<?php
declare(strict_types=1);

/**
 * One-time, insert-only intake for snapshots exported by scanner-ingest.
 * Preview and apply use the same planner; writes are transactional, serialized
 * per SoR database, provenance-stamped, and never update existing inventory.
 */
final class ScannerImport
{
    public const MAX_REQUEST_BYTES = 4 * 1024 * 1024;
    private const SOURCE = 'scanner-ingest';
    private const INDEX_FIELDS = [
        'sources' => ['id','slug'],
        'hardware_models' => ['id','make','model'],
        'hardware_units' => ['id','asset_tag','serial'],
        'locations' => ['id','name','deleted_at'],
        'external_refs' => ['source_id','subject_table','subject_id','external_id'],
        'barcodes' => ['code','deleted_at','subject_table','subject_id'],
    ];
    private const ITEM_FIELDS = ['id','name','manufacturer','model','category','barcode','notes','created_at','updated_at','version','archived'];
    private const ASSET_FIELDS = ['id','item_id','tag','serial','location','status','notes','created_at','updated_at','version','archived'];
    private const STOCK = ['in_stock' => 'in_stock', 'in_use' => 'deployed', 'retired' => 'retired'];

    private PDO $db;
    private string $operator;
    private array $mapping;
    private ?string $repairStatus;
    private string $unmappedLocation;
    private array $rows = [];
    private array $rowIndex = [];
    private array $indexWeightCache = [];
    private array $weightStatements = [];
    private array $columns = [];
    private array $enums = [];
    private array $operations = [];
    private array $counts = [];
    private array $warnings = [];
    private array $locationMatches = [];
    private array $source = [];
    private int $nextVirtual = -1;

    private function __construct(PDO $db, string $operator, array $mapping, ?string $repairStatus, string $unmappedLocation)
    {
        $this->db = $db;
        $this->operator = $operator;
        $this->mapping = $mapping;
        $this->repairStatus = $repairStatus;
        $this->unmappedLocation = $unmappedLocation;
    }

    /** @return array{mode:string,counts:array<string,int>,auditRows:int,warnings:list<string>,fingerprint:string} */
    public static function run(PDO $db, string $operator, array $snapshot, array $mapping = [], bool $apply = false,
                               ?string $repairStatus = null, string $unmappedLocation = 'error',
                               ?string $expectedFingerprint = null): array
    {
        if ($operator === '' || strlen($operator) > 64) {
            throw new InvalidArgumentException('A named operator is required.');
        }
        self::validateSnapshot($snapshot);
        self::validateMapping($mapping);
        if ($repairStatus !== null && !in_array($repairStatus, ['in_stock', 'retired'], true)) {
            throw new InvalidArgumentException('repairStatus must be in_stock or retired.');
        }
        if (!in_array($unmappedLocation, ['error', 'notes'], true)) {
            throw new InvalidArgumentException('unmappedLocation must be error or notes.');
        }

        $instance = new self($db, $operator, $mapping, $repairStatus, $unmappedLocation);
        $lockName = null;
        try {
            if ($apply) {
                $dbName = (string) $db->query('SELECT DATABASE()')->fetchColumn();
                if ($dbName === '') throw new RuntimeException('SoR database name is unavailable.');
                $lockName = 'scanner-import:' . substr(hash('sha256', $dbName), 0, 32);
                $st = $db->prepare('SELECT GET_LOCK(:name, 0)');
                $st->execute([':name' => $lockName]);
                if ((int) $st->fetchColumn() !== 1) throw new RuntimeException('Another scanner import is running.');
                $db->exec('SET TRANSACTION ISOLATION LEVEL SERIALIZABLE');
                $db->beginTransaction();
            } else {
                $db->exec('SET TRANSACTION ISOLATION LEVEL REPEATABLE READ, READ ONLY');
                $db->beginTransaction();
            }

            $instance->loadRows($apply);
            $instance->prepareIndexWeights($snapshot);
            $instance->build($snapshot);
            $fingerprint = $instance->fingerprint();
            if ($apply && $expectedFingerprint !== null && !hash_equals($fingerprint, $expectedFingerprint)) {
                throw new RuntimeException('SoR state or import plan changed since preview; preview again before applying.');
            }
            if ($apply) {
                $instance->apply();
                $db->commit();
            } else {
                $db->rollBack();
            }
            ksort($instance->counts);
            $warnings = array_keys($instance->warnings);
            sort($warnings);
            return ['mode' => $apply ? 'applied' : 'preview', 'counts' => $instance->counts,
                    'auditRows' => count($instance->operations), 'warnings' => $warnings, 'fingerprint' => $fingerprint,
                    'locations' => $instance->locationMatches];
        } catch (Throwable $e) {
            if ($db->inTransaction()) $db->rollBack();
            throw $e;
        } finally {
            if ($lockName !== null) {
                $st = $db->prepare('SELECT RELEASE_LOCK(:name)');
                $st->execute([':name' => $lockName]);
            }
        }
    }

    private static function validateSnapshot(array $snapshot): void
    {
        if (($snapshot['schema_version'] ?? null) !== 1 || !is_array($snapshot['items'] ?? null) || !array_is_list($snapshot['items']) ||
            !is_array($snapshot['assets'] ?? null) || !array_is_list($snapshot['assets']) || count($snapshot['items']) > 1000 || count($snapshot['assets']) > 5000) {
            throw new InvalidArgumentException('Expected a scanner-ingest JSON snapshot (schema_version 1, with items and assets arrays).');
        }
        $items = [];
        foreach ($snapshot['items'] as $row) {
            self::validateSourceRow($row, self::ITEM_FIELDS, 'item');
            if (isset($items[$row['id']])) throw new InvalidArgumentException('Duplicate scanner item UUID.');
            $items[$row['id']] = $row;
        }
        $assets = [];
        foreach ($snapshot['assets'] as $row) {
            self::validateSourceRow($row, self::ASSET_FIELDS, 'asset');
            if (isset($assets[$row['id']])) throw new InvalidArgumentException('Duplicate scanner asset UUID.');
            if (!isset($items[$row['item_id']]) || ((int) $row['archived'] === 0 && (int) $items[$row['item_id']]['archived'] === 1)) {
                throw new InvalidArgumentException('Scanner asset references a missing or archived catalog item.');
            }
            $assets[$row['id']] = $row;
        }
    }

    private static function validateSourceRow($row, array $fields, string $label): void
    {
        if (!is_array($row) || array_diff($fields, array_keys($row))) throw new InvalidArgumentException("Invalid scanner $label row.");
        if (!is_string($row['id']) || preg_match('/\A[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\z/', $row['id']) !== 1) {
            throw new InvalidArgumentException("Invalid lowercase scanner $label UUID.");
        }
        foreach ($fields as $field) {
            if (in_array($field, ['version', 'archived'], true)) continue;
            if (!is_string($row[$field])) throw new InvalidArgumentException("Scanner $label $field must be text.");
        }
        if (!is_int($row['version']) || $row['version'] < 1 || !is_int($row['archived']) || !in_array($row['archived'], [0, 1], true)) {
            throw new InvalidArgumentException("Invalid scanner $label version/archive state.");
        }
    }

    private static function validateMapping(array $mapping): void
    {
        if (array_diff(array_keys($mapping), ['locations', 'models', 'categories'])) throw new InvalidArgumentException('Unknown scanner import mapping section.');
        foreach (['locations', 'models', 'categories'] as $section) {
            $rows = $mapping[$section] ?? [];
            if (!is_array($rows)) throw new InvalidArgumentException("$section mapping must be an object.");
            foreach ($rows as $key => $value) {
                $validKey = is_string($key) || (in_array($section, ['locations', 'categories'], true) && is_int($key));
                if (!$validKey || ($section === 'categories' ? !is_string($value) : (!is_int($value) || $value < 1))) {
                    throw new InvalidArgumentException("Invalid $section mapping entry.");
                }
            }
        }
    }

    private function loadRows(bool $lock): void
    {
        $required = [
            'sources' => ['id','slug','display_name','kind','deleted_at'],
            'hardware_models' => ['id','make','model','hw_type','notes','source_id','asserted_kind','asserted_at','deleted_at'],
            'hardware_units' => ['id','model_id','serial','asset_tag','location_id','quantity','stock_status','notes','source_id','asserted_kind','asserted_at','deleted_at'],
            'locations' => ['id','name','deleted_at'],
            'external_refs' => ['id','source_id','subject_table','subject_id','external_id','asserted_kind','asserted_at','deleted_at'],
            'barcodes' => ['id','code','code_type','symbology','subject_table','subject_id','notes','source_id','asserted_kind','asserted_at','deleted_at'],
            'audit_log' => ['id','table_name','row_id','action','actor_label','source_id','asserted_kind','new_values','note'],
        ];
        foreach ($required as $table => $fields) {
            $engine = $this->db->prepare('SELECT ENGINE FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME=:table');
            $engine->execute([':table' => $table]);
            if ($engine->fetchColumn() !== 'InnoDB') throw new RuntimeException("SoR table $table is missing or not transactional.");
            $meta = $this->db->prepare('SELECT COLUMN_NAME,COLUMN_TYPE,CHARACTER_MAXIMUM_LENGTH,COLLATION_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME=:table');
            $meta->execute([':table' => $table]);
            foreach ($meta->fetchAll(PDO::FETCH_ASSOC) as $column) {
                $this->columns[$table][$column['COLUMN_NAME']] = $column;
                if (str_starts_with($column['COLUMN_TYPE'], 'enum(')) {
                    preg_match_all("/'((?:[^'\\\\]|\\\\.)*)'/", $column['COLUMN_TYPE'], $matches);
                    $this->enums[$table][$column['COLUMN_NAME']] = array_map(static fn($s) => stripcslashes($s), $matches[1]);
                }
            }
            if (array_diff($fields, array_keys($this->columns[$table] ?? []))) throw new RuntimeException("SoR table $table has an unsupported schema.");
            if ($table !== 'audit_log') {
                $select = array_map(static fn($f) => '`' . $f . '`', array_keys($this->columns[$table]));
                foreach (self::INDEX_FIELDS[$table] ?? [] as $field) {
                    if ($this->indexCollation($table, $field) !== null && !($table === 'locations' && $field === 'name')) {
                        $select[] = 'HEX(WEIGHT_STRING(`' . $field . '`)) AS `__idx_' . $field . '`';
                    }
                }
                $query = 'SELECT ' . implode(',', $select) . " FROM `$table`" . ($lock ? ' FOR UPDATE' : '');
                $this->rows[$table] = $this->db->query($query)->fetchAll(PDO::FETCH_ASSOC);
                foreach ($this->rows[$table] as $index => &$row) {
                    $weights = [];
                    foreach (self::INDEX_FIELDS[$table] ?? [] as $field) {
                        $alias = '__idx_' . $field;
                        if (array_key_exists($alias, $row)) { $weights[$field] = $row[$alias]; unset($row[$alias]); }
                    }
                    $this->indexRow($table, $row, $index, $weights);
                }
                unset($row);
            }
        }
    }

    private function indexCollation(string $table, string $field): ?string
    {
        $collation = $this->columns[$table][$field]['COLLATION_NAME'] ?? null;
        if ($collation !== null && !preg_match('/\Autf8mb4_[A-Za-z0-9_]+\z/', $collation)) {
            throw new RuntimeException("Unsupported SoR collation on $table.$field.");
        }
        return $collation;
    }

    private function indexWeight(string $collation, string $value): string
    {
        $key = $collation . "\0" . $value;
        if (!array_key_exists($key, $this->indexWeightCache)) {
            $this->weightStatements[$collation] ??= $this->db->prepare(
                'SELECT HEX(WEIGHT_STRING(CONVERT(:value USING utf8mb4) COLLATE ' . $collation . '))'
            );
            $this->weightStatements[$collation]->execute([':value' => $value]);
            $this->indexWeightCache[$key] = (string) $this->weightStatements[$collation]->fetchColumn();
        }
        return $this->indexWeightCache[$key];
    }

    private function indexKey(string $table, string $field, $value, ?string $weight = null): string
    {
        if ($value === null) return 'null:';
        if (is_string($value)) {
            $collation = $this->indexCollation($table, $field);
            if ($collation !== null && !($table === 'locations' && $field === 'name')) {
                return 'weight:' . ($weight ?? $this->indexWeight($collation, $value));
            }
        }
        return 'value:' . (string) $value;
    }

    private function prepareIndexWeights(array $snapshot): void
    {
        $values = [];
        $add = function (string $table, string $field, string $value) use (&$values): void {
            $collation = $this->indexCollation($table, $field);
            if ($collation === null || ($table === 'locations' && $field === 'name')) return;
            $values[$collation][base64_encode($value)] = $value;
        };
        $add('sources', 'slug', self::SOURCE);
        foreach ($snapshot['items'] as $item) {
            if ($item['archived'] === 1) continue;
            $add('hardware_models', 'make', $item['manufacturer'] !== '' ? $item['manufacturer'] : 'Unknown');
            $add('hardware_models', 'model', $item['model'] !== '' ? $item['model'] : $item['name']);
            $add('external_refs', 'external_id', $item['id']);
        }
        foreach ($snapshot['assets'] as $asset) {
            if ($asset['archived'] === 1) continue;
            $add('external_refs', 'external_id', $asset['id']);
            $add('hardware_units', 'asset_tag', $asset['tag']);
            $add('hardware_units', 'serial', $asset['serial']);
            if ($asset['tag'] !== '') $add('barcodes', 'code', $asset['tag']);
            if ($asset['serial'] !== '') $add('barcodes', 'code', $asset['serial']);
        }
        foreach ($values as $collation => $uniqueValues) {
            $entries = array_values($uniqueValues);
            foreach (array_chunk($entries, 100) as $batch) {
                $parts = [];
                $params = [];
                foreach ($batch as $index => $value) {
                    $input = ':i' . $index;
                    $weight = ':w' . $index;
                    $parts[] = 'SELECT ' . $input . ' AS input, HEX(WEIGHT_STRING(CONVERT(' . $weight . ' USING utf8mb4) COLLATE ' . $collation . ')) AS weight';
                    $params[$input] = $value;
                    $params[$weight] = $value;
                }
                $st = $this->db->prepare(implode(' UNION ALL ', $parts));
                $st->execute($params);
                foreach ($st->fetchAll(PDO::FETCH_ASSOC) as $row) {
                    $this->indexWeightCache[$collation . "\0" . $row['input']] = (string) $row['weight'];
                }
            }
        }
    }

    private function indexRow(string $table, array $row, int $index, array $weights = []): void
    {
        foreach (self::INDEX_FIELDS[$table] ?? [] as $field) {
            $this->rowIndex[$table][$field][$this->indexKey($table, $field, $row[$field] ?? null, $weights[$field] ?? null)][] = $index;
        }
    }

    private function equal(string $table, string $field, $left, $right): bool
    {
        if ($left === null || $right === null) return $left === $right;
        if ($table === 'locations' && $field === 'name') return is_string($left) && is_string($right) && $left === $right;
        if (!is_string($left) || !is_string($right) || $left === $right) return $left === $right || (string) $left === (string) $right;
        $collation = $this->columns[$table][$field]['COLLATION_NAME'] ?? null;
        if (!$collation) return $left === $right;
        if (!preg_match('/\Autf8mb4_[A-Za-z0-9_]+\z/', $collation)) throw new RuntimeException("Unsupported SoR collation on $table.$field.");
        $st = $this->db->prepare("SELECT CONVERT(:l USING utf8mb4) COLLATE $collation = CONVERT(:r USING utf8mb4) COLLATE $collation");
        $st->execute([':l' => $left, ':r' => $right]);
        return (bool) $st->fetchColumn();
    }

    private function find(string $table, array $criteria): array
    {
        $candidates = null;
        foreach ($criteria as $field => $value) {
            $matches = $this->rowIndex[$table][$field][$this->indexKey($table, $field, $value)] ?? [];
            if (!$matches) return [];
            if ($candidates === null || count($matches) < count($candidates)) $candidates = $matches;
        }
        $found = [];
        foreach ($candidates ?? [] as $index) {
            $row = $this->rows[$table][$index];
            $ok = true;
            foreach ($criteria as $field => $value) {
                if (!$this->equal($table, $field, $row[$field] ?? null, $value)) { $ok = false; break; }
            }
            if ($ok) $found[] = $row;
        }
        return $found;
    }

    private function one(array $rows, string $label): ?array
    {
        if (count($rows) > 1) throw new RuntimeException("Ambiguous $label; multiple SoR records match.");
        if ($rows && ($rows[0]['deleted_at'] ?? null) !== null) throw new RuntimeException("$label matches a soft-deleted SoR record; reconcile it first.");
        return $rows[0] ?? null;
    }

    private function count(string $key, int $delta = 1): void
    {
        $this->counts[$key] = ($this->counts[$key] ?? 0) + $delta;
    }

    private function check(string $table, array $values): void
    {
        foreach ($values as $field => $value) {
            if (!is_string($value) || !isset($this->columns[$table][$field])) continue;
            $meta = $this->columns[$table][$field];
            $max = $meta['CHARACTER_MAXIMUM_LENGTH'];
            $length = str_contains(strtolower($meta['COLUMN_TYPE']), 'text') ? strlen($value) : preg_match_all('/./us', $value);
            if ($max !== null && $length > (int) $max) throw new RuntimeException("$table.$field exceeds its destination size.");
            if (isset($this->enums[$table][$field]) && !in_array($value, $this->enums[$table][$field], true)) throw new RuntimeException("$table.$field does not support $value.");
        }
    }

    private function insert(string $table, array $values): array
    {
        $this->check($table, $values);
        $id = $this->nextVirtual--;
        $row = array_merge($values, ['id' => $id, 'deleted_at' => null]);
        $this->operations[] = ['table' => $table, 'id' => $id, 'values' => $values];
        $index = count($this->rows[$table]);
        $this->rows[$table][] = $row;
        $this->indexRow($table, $row, $index);
        $this->count('insert.' . $table);
        return $row;
    }

    private function provenance(): array
    {
        return ['source_id' => $this->source['id'], 'asserted_kind' => 'human', 'asserted_at' => gmdate('Y-m-d H:i:s')];
    }

    private function reference(string $table, string $externalId): ?array
    {
        $ref = $this->one($this->find('external_refs', ['source_id' => $this->source['id'], 'subject_table' => $table, 'external_id' => $externalId]), "external reference $externalId");
        if (!$ref) return null;
        $row = $this->one($this->find($table, ['id' => $ref['subject_id']]), "referenced $table row");
        if (!$row) throw new RuntimeException("Dangling scanner external reference $externalId.");
        return $row;
    }

    private function addReference(string $table, string $externalId, array $target): void
    {
        $this->insert('external_refs', array_merge($this->provenance(), ['subject_table' => $table, 'subject_id' => $target['id'], 'external_id' => $externalId]));
    }

    private function location(array $asset): ?int
    {
        $name = $asset['location'];
        if ($name === '') return null;
        $mapped = $this->mapping['locations'][$name] ?? null;
        $matches = $mapped ? $this->find('locations', ['id' => $mapped]) : $this->find('locations', ['name' => $name, 'deleted_at' => null]);
        $row = $this->one($matches, "location for scanner asset {$asset['id']}");
        if ($row) {
            $this->locationMatches[$name] = ['id' => (int) $row['id'], 'name' => (string) $row['name']];
            return (int) $row['id'];
        }
        if ($mapped || $this->unmappedLocation === 'error') throw new RuntimeException("No unique live SoR location for scanner location '$name'; select a location or explicitly keep it in notes.");
        $this->locationMatches[$name] = ['id' => null, 'name' => null];
        $this->warnings['Some source locations are preserved only in notes; imported location_id will be NULL.'] = true;
        return null;
    }

    private function model(array $item): array
    {
        $previous = $this->reference('hardware_models', $item['id']);
        $mapped = $this->mapping['models'][$item['id']] ?? null;
        if ($mapped) {
            $row = $this->one($this->find('hardware_models', ['id' => $mapped]), "model mapping for {$item['id']}");
            if (!$row || ($previous && (int) $row['id'] !== (int) $previous['id'])) throw new RuntimeException("Model mapping is missing or conflicts with an earlier import for {$item['id']}.");
        } else {
            $make = $item['manufacturer'] !== '' ? $item['manufacturer'] : 'Unknown';
            $model = $item['model'] !== '' ? $item['model'] : $item['name'];
            if ($model === '') throw new RuntimeException("Scanner catalog item {$item['id']} needs a name or model.");
            $this->check('hardware_models', ['make' => $make, 'model' => $model]);
            $row = $previous ?: $this->one($this->find('hardware_models', ['make' => $make, 'model' => $model]), "make/model for scanner item {$item['id']}");
            if ($previous && ($previous['make'] !== $make || $previous['model'] !== $model)) throw new RuntimeException("Imported model identity changed for {$item['id']}; reconcile it before importing.");
            if (!$row) {
                $hwType = $this->mapping['categories'][$item['category']] ?? strtolower($item['category']);
                if (!in_array($hwType, $this->enums['hardware_models']['hw_type'] ?? [], true)) {
                    if (isset($this->mapping['categories'][$item['category']])) throw new RuntimeException("Explicit hardware type mapping is invalid for scanner item {$item['id']}.");
                    $hwType = 'other';
                }
                $row = $this->insert('hardware_models', array_merge($this->provenance(), ['make' => $make, 'model' => $model, 'hw_type' => $hwType, 'notes' => self::canonicalJson(['scanner_ingest_item' => $item])]));
            }
        }
        if (!$previous) $this->addReference('hardware_models', $item['id'], $row);
        if ((int) $row['id'] > 0) $this->count('reuse.hardware_models');
        return $row;
    }

    private function barcode(string $code, string $type, array $unit): void
    {
        if ($code === '') return;
        $row = $this->one($this->find('barcodes', ['code' => $code, 'deleted_at' => null]), "barcode for unit {$unit['asset_tag']}");
        if ($row) {
            if ($row['subject_table'] !== 'hardware_units' || (int) $row['subject_id'] !== (int) $unit['id'] || $row['code'] !== $code) throw new RuntimeException("Barcode collision for unit {$unit['asset_tag']}.");
            $this->count('reuse.barcodes');
            return;
        }
        foreach (['serial', 'asset_tag'] as $field) {
            foreach ($this->find('hardware_units', [$field => $code]) as $other) {
                if ((int) $other['id'] !== (int) $unit['id']) throw new RuntimeException("Code conflicts with another hardware unit for {$unit['asset_tag']}.");
            }
        }
        $notes = $type === 'asset_tag' ? 'Imported scanner code. Original SI label is Code 39; SoR reprints use Code 128.' : 'Imported manufacturer serial.';
        $this->insert('barcodes', array_merge($this->provenance(), ['code' => $code, 'code_type' => $type, 'symbology' => 'code128', 'subject_table' => 'hardware_units', 'subject_id' => $unit['id'], 'notes' => $notes]));
    }

    private function build(array $snapshot): void
    {
        $source = $this->one($this->find('sources', ['slug' => self::SOURCE]), 'scanner-ingest source');
        $this->source = $source ?: $this->insert('sources', ['slug' => self::SOURCE, 'display_name' => 'Standalone scanner ingest', 'kind' => 'seed']);
        $items = [];
        $models = [];
        foreach ($snapshot['items'] as $item) {
            if ($item['archived'] === 1) { $this->count('skip.archived_items'); continue; }
            $items[$item['id']] = $item;
            $models[$item['id']] = $this->model($item);
        }
        foreach ($snapshot['assets'] as $asset) {
            if ($asset['archived'] === 1) { $this->count('skip.archived_assets'); continue; }
            $status = $asset['status'] === 'repair' ? $this->repairStatus : (self::STOCK[$asset['status']] ?? null);
            if ($status === null) throw new RuntimeException("Scanner asset {$asset['id']} has repair status; choose in_stock or retired before import.");
            if ($asset['tag'] === '') throw new RuntimeException("Scanner asset {$asset['id']} has no asset tag.");
            $item = $items[$asset['item_id']] ?? null;
            if (!$item || !isset($models[$asset['item_id']])) continue;
            $desired = ['model_id' => $models[$asset['item_id']]['id'], 'serial' => $asset['serial'] !== '' ? $asset['serial'] : null,
                'asset_tag' => $asset['tag'], 'location_id' => $this->location($asset), 'quantity' => 1, 'stock_status' => $status,
                'notes' => self::canonicalJson(['scanner_ingest_asset' => $asset, 'scanner_ingest_item' => $item])];
            $this->check('hardware_units', $desired);
            $this->check('barcodes', ['code' => $asset['tag']]);
            if ($asset['serial'] !== '') $this->check('barcodes', ['code' => $asset['serial']]);
            $existing = $this->reference('hardware_units', $asset['id']);
            if ($existing) {
                foreach ($desired as $field => $value) if (($existing[$field] ?? null) != $value) throw new RuntimeException("Previously imported scanner asset {$asset['id']} differs from SoR state; no existing inventory is overwritten.");
                $unit = $existing;
                $this->count('reuse.hardware_units');
            } else {
                $collisions = $this->find('hardware_units', ['asset_tag' => $asset['tag']]);
                if ($asset['serial'] !== '') $collisions = array_merge($collisions, $this->find('hardware_units', ['serial' => $asset['serial']]));
                if ($collisions) throw new RuntimeException("Scanner asset {$asset['id']} collides with existing SoR inventory; reconcile it before import.");
                $unit = $this->insert('hardware_units', array_merge($desired, $this->provenance()));
                $this->addReference('hardware_units', $asset['id'], $unit);
            }
            $this->barcode($asset['tag'], 'asset_tag', $unit);
            if ($asset['serial'] !== $asset['tag']) $this->barcode($asset['serial'], 'serial', $unit);
        }
        $this->warnings['Shared product barcodes and original metadata remain in notes; SoR codes bind to individual units.'] = true;
    }

    private function apply(): void
    {
        $ids = [];
        foreach ($this->operations as $operation) {
            $values = $operation['values'];
            foreach ($values as $key => $value) if (is_int($value) && isset($ids[$value])) $values[$key] = $ids[$value];
            $names = array_keys($values);
            $sql = 'INSERT INTO `' . $operation['table'] . '` (' . implode(',', array_map(static fn($n) => '`' . $n . '`', $names)) . ') VALUES (' . implode(',', array_map(static fn($n) => ':' . $n, $names)) . ')';
            $st = $this->db->prepare($sql);
            foreach ($values as $key => $value) $st->bindValue(':' . $key, $value);
            $st->execute();
            $ids[$operation['id']] = (int) $this->db->lastInsertId();
            $sourceId = $ids[$this->source['id']] ?? (int) $this->source['id'];
            $audit = $this->db->prepare('INSERT INTO audit_log (table_name,row_id,action,actor_label,source_id,asserted_kind,new_values,note) VALUES (:table,:id,\'insert\',:actor,:source,\'human\',:values,:note)');
            $audit->execute([':table' => $operation['table'], ':id' => $ids[$operation['id']], ':actor' => 'dashboard:' . $this->operator,
                ':source' => $sourceId, ':values' => json_encode($values, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE), ':note' => 'Scanner-ingest snapshot import']);
        }
    }

    private static function canonicalValue($value)
    {
        if (is_array($value)) {
            if (!array_is_list($value)) ksort($value, SORT_STRING);
            foreach ($value as $key => $child) $value[$key] = self::canonicalValue($child);
        }
        return $value;
    }

    private static function canonicalJson($value): string
    {
        return json_encode(self::canonicalValue($value), JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE | JSON_THROW_ON_ERROR);
    }

    private function fingerprint(): string
    {
        $counts = $this->counts;
        ksort($counts, SORT_STRING);
        $warnings = array_keys($this->warnings);
        sort($warnings, SORT_STRING);
        $locations = $this->locationMatches;
        ksort($locations, SORT_STRING);
        $context = hash_init('sha256');
        hash_update($context, '{"counts":' . self::canonicalJson($counts)
            . ',"locations":' . self::canonicalJson($locations) . ',"operations":[');
        foreach ($this->operations as $index => $operation) {
            if ($index > 0) hash_update($context, ',');
            unset($operation['values']['asserted_at']);
            hash_update($context, self::canonicalJson($operation));
        }
        hash_update($context, '],"warnings":' . self::canonicalJson($warnings) . '}');
        return hash_final($context);
    }
}
