<?php
declare(strict_types=1);

/** Product catalog API. Loaded after inventory.php for its Inv and JSON helpers. */
return static function (Router $router): void {
    $session = Policy::INVENTORY_CATALOG_READ;
    $operator = Policy::INVENTORY_CATALOG_WRITE; // Policy::enforce also requires same-origin CSRF.

    $router->get('/api/inventory/product-types', static function (): void {
        $where = ['pt.deleted_at IS NULL']; $params = [];
        if (isset($_GET['q'])) {
            if (!is_string($_GET['q'])) Response::error('bad_request', 'q must be a string.', 400);
            $q = trim($_GET['q']);
            if ($q !== '') { $where[] = '(pt.name LIKE :q OR pt.brand LIKE :q OR pt.model LIKE :q OR pt.category LIKE :q)'; $params[':q'] = '%'.$q.'%'; }
        }
        if (isset($_GET['kind']) && $_GET['kind'] !== '') {
            $kind = $_GET['kind'];
            if (!is_string($kind) || !in_array($kind, ['hardware','peripheral','component','software','accessory','consumable','other'], true)) Response::error('bad_request', 'kind is invalid.', 400);
            $where[] = 'pt.kind = :kind'; $params[':kind'] = $kind;
        }
        $limit = catalog_page_int($_GET['limit'] ?? 100, 1, 500);
        $offset = catalog_page_int($_GET['offset'] ?? 0, 0, 2147483647);
        $predicate = implode(' AND ', $where);
        $items = Inv::rows('SELECT pt.id,pt.kind,pt.name,pt.brand,pt.model,pt.category,pt.specs,pt.notes,pt.image_url,pt.lookup_source,pt.lookup_ref,pt.created_at,pt.updated_at FROM product_types pt WHERE '.$predicate.' ORDER BY pt.name,pt.id LIMIT '.$limit.' OFFSET '.$offset, $params);
        $total = (int)Inv::scalar('SELECT COUNT(*) FROM product_types pt WHERE '.$predicate, $params);
        foreach ($items as &$item) { $item = catalog_type_row($item); } unset($item);
        Response::ok(inv_page_result($items, $total, ['limit'=>$limit,'offset'=>$offset]));
    }, $session);

    $router->get('/api/inventory/product-types/{id}', static function (array $p): void {
        $id = Inv::idParam($p['id']);
        $row = Inv::row('SELECT id,kind,name,brand,model,category,specs,notes,image_url,lookup_source,lookup_ref,created_at,updated_at FROM product_types WHERE id=:id AND deleted_at IS NULL', [':id'=>$id]);
        if ($row === null) Response::error('not_found', "No product type $id", 404);
        Response::ok(catalog_type_row($row));
    }, $session);

    $router->add('DELETE', '/api/inventory/product-types/{id}', static function (array $p): void {
        $id=Inv::idParam($p['id']); $db=Sor::db();
        $row=Inv::row('SELECT id,kind,name,brand,model,category FROM product_types WHERE id=:id AND deleted_at IS NULL',[':id'=>$id]);
        if($row===null)Response::error('not_found',"No product type $id",404);
        $source=Sor::dashboardSourceId(); $db->beginTransaction();
        try{
            // D5: in-use types may be retired; existing items keep their reference.
            $db->prepare("UPDATE product_types SET deleted_at=NOW(),source_id=:source,asserted_kind='human',asserted_at=NOW() WHERE id=:id AND deleted_at IS NULL")->execute([':source'=>$source,':id'=>$id]);catalog_audit($db,'product_types',$id,'soft_delete',$row,['deleted_at'=>'now'],$source);
            // Free the type's codes for reuse; a dead type's barcode must not block a live one.
            $codes=$db->prepare('SELECT id,product_type_id,code,code_type FROM product_barcodes WHERE product_type_id=:id AND deleted_at IS NULL');$codes->execute([':id'=>$id]);
            foreach($codes->fetchAll(PDO::FETCH_ASSOC) as $code){$db->prepare("UPDATE product_barcodes SET deleted_at=NOW(),source_id=:source,asserted_kind='human',asserted_at=NOW() WHERE id=:id")->execute([':source'=>$source,':id'=>(int)$code['id']]);catalog_audit($db,'product_barcodes',(int)$code['id'],'soft_delete',$code,['deleted_at'=>'now'],$source);}
            $db->commit();Response::ok(['id'=>$id,'status'=>'deleted']);}
        catch(Throwable $e){if($db->inTransaction())$db->rollBack();throw $e;}
    }, $operator);

    $router->post('/api/inventory/product-types', static function (): void {
        $body = solari_json_body(); $barcode = catalog_barcode_input($body); unset($body['barcode']); $v = catalog_validate_type($body, false);
        $db = Sor::db(); $source = Sor::dashboardSourceId();
        try {
            $db->beginTransaction();
            $st = $db->prepare("INSERT INTO product_types(kind,name,brand,model,category,specs,notes,image_url,source_id,asserted_kind,asserted_at) VALUES(:kind,:name,:brand,:model,:category,:specs,:notes,:image_url,:source,'human',NOW())");
            $st->execute($v + [':source'=>$source]); $id = (int)$db->lastInsertId();
            catalog_audit($db, 'product_types', $id, 'insert', null, catalog_audit_values($v), $source);
            if ($barcode !== null) catalog_insert_barcode($db, $barcode, $id, $source);
            $db->commit();
            Response::ok(catalog_get_type($id), 201);
        } catch (PDOException $e) {
            if ($db->inTransaction()) $db->rollBack();
            if (($e->errorInfo[1] ?? null) === 1062) Response::error('duplicate_barcode', 'That barcode is already attached to a live product type.', 409);
            error_log('product type create failed: '.$e->getMessage()); Response::error('service_unavailable', 'Inventory storage is unavailable.', 503);
        } catch (Throwable $e) {
            if ($db->inTransaction()) $db->rollBack(); throw $e;
        }
    }, $operator);

    $router->add('PUT', '/api/inventory/product-types/{id}', static function (array $p): void {
        $id = Inv::idParam($p['id']); $db = Sor::db(); $body = solari_json_body();
        if ($body === []) Response::error('bad_request', 'At least one field is required.', 400);
        $expected = $body['updated_at'] ?? null;
        unset($body['updated_at']); $v = catalog_validate_type($body, true);
        $sets=[]; $params=[':id'=>$id, ':source'=>Sor::dashboardSourceId()];
        $old=[]; $new=[];
        foreach ($v as $key=>$value) { $column=substr($key,1); $sets[]="$column=$key"; $params[$key]=$value; $new[$column]=$value; }
        if (!$sets) Response::error('bad_request', 'No supported fields supplied.', 400);
        $sets[]="source_id=:source"; $sets[]="asserted_kind='human'"; $sets[]='asserted_at=NOW()';
        $db->beginTransaction();
        try {
            $lock = $db->prepare('SELECT id,kind,name,brand,model,category,specs,notes,image_url,updated_at FROM product_types WHERE id=:id AND deleted_at IS NULL FOR UPDATE'); $lock->execute([':id'=>$id]);
            $current = $lock->fetch(PDO::FETCH_ASSOC);
            if (!is_array($current)) { $db->rollBack(); Response::error('not_found', "No product type $id", 404); }
            if ($expected !== null && $expected !== $current['updated_at']) { $db->rollBack(); Response::error('conflict', 'Product type changed since it was read.', 409); }
            foreach (array_keys($new) as $column) $old[$column] = $current[$column] ?? null;
            $db->prepare('UPDATE product_types SET '.implode(',',$sets).' WHERE id=:id AND deleted_at IS NULL')->execute($params);
            $fresh = catalog_get_type($id);
            // Preserve the legacy provisioning/RackWire projection when a linked legacy model exists.
            $legacy = $db->prepare('SELECT id,hw_type FROM hardware_models WHERE product_type_id=:id AND deleted_at IS NULL'); $legacy->execute([':id'=>$id]);
            $legacyRow = $legacy->fetch(PDO::FETCH_ASSOC);
            if (is_array($legacyRow)) {
                $legacyOld = Inv::row('SELECT id,make,model,hw_type,notes FROM hardware_models WHERE id=:id', [':id'=>(int)$legacyRow['id']]);
                $db->prepare("UPDATE hardware_models SET make=:brand,model=:model,notes=:notes,hw_type=:hw_type,source_id=:source,asserted_kind='human',asserted_at=NOW() WHERE id=:legacy_id")
                    ->execute([':brand'=>$fresh['brand'], ':model'=>$fresh['model'] ?? $fresh['name'], ':notes'=>$fresh['notes'], ':hw_type'=>catalog_legacy_hw_type($fresh['kind'], (string)$legacyRow['hw_type']), ':source'=>$params[':source'], ':legacy_id'=>(int)$legacyRow['id']]);
                $legacyNew = Inv::row('SELECT make,model,hw_type,notes FROM hardware_models WHERE id=:id', [':id'=>(int)$legacyRow['id']]);
                catalog_audit($db, 'hardware_models', (int)$legacyRow['id'], 'update', $legacyOld, $legacyNew, (int)$params[':source']);
                // The 033 legacy sync trigger rewrites kind/name/brand/model/notes from the projection.
                // Restore the whole authoritative row, not just this request's fields, or an earlier
                // operator edit (e.g. name != model) would be reverted by an unrelated PUT.
                $db->prepare('UPDATE product_types SET kind=:kind,name=:name,brand=:brand,model=:model,notes=:notes WHERE id=:id')
                    ->execute([':kind'=>$fresh['kind'], ':name'=>$fresh['name'], ':brand'=>$fresh['brand'], ':model'=>$fresh['model'], ':notes'=>$fresh['notes'], ':id'=>$id]);
            }
            $fresh = catalog_get_type($id);
            catalog_audit($db, 'product_types', $id, 'update', $old, $new, (int)$params[':source']);
            $db->commit(); Response::ok($fresh);
        } catch (PDOException $e) {
            if ($db->inTransaction()) $db->rollBack();
            if (($e->errorInfo[1] ?? null) === 1062) Response::error('conflict', 'Product type conflicts with another live record.', 409);
            error_log('product type update failed: '.$e->getMessage()); Response::error('service_unavailable', 'Inventory storage is unavailable.', 503);
        } catch (Throwable $e) {
            if ($db->inTransaction()) $db->rollBack(); throw $e;
        }
    }, $operator);

    $router->get('/api/inventory/product-barcodes', static function (): void {
        if (!isset($_GET['code']) || !is_string($_GET['code'])) Response::error('bad_request', 'code is required.', 400);
        $code = catalog_normalize_code($_GET['code']); if ($code === '') Response::error('bad_request', 'code must not be empty.', 400);
        $row = Inv::row('SELECT pb.id AS barcode_id,pb.code,pb.code_type,pt.id,pt.kind,pt.name,pt.brand,pt.model,pt.category,pt.specs,pt.notes,pt.image_url FROM product_barcodes pb JOIN product_types pt ON pt.id=pb.product_type_id AND pt.deleted_at IS NULL WHERE pb.code=:code AND pb.deleted_at IS NULL', [':code'=>$code]);
        Response::ok($row === null ? ['found'=>false,'productType'=>null] : ['found'=>true,'productType'=>catalog_type_row($row),'barcode'=>['id'=>(int)$row['barcode_id'],'code'=>$row['code'],'code_type'=>$row['code_type']]]);
    }, $session);

    $router->post('/api/inventory/product-barcodes', static function (): void {
        $body = solari_json_body(); $typeId=Inv::idParam((string)($body['product_type_id'] ?? ''));
        $code=catalog_normalize_code(is_string($body['code'] ?? null) ? $body['code'] : '');
        $type=$body['code_type'] ?? 'other';
        if ($code==='' || strlen($code)>128 || !in_array($type,['gtin','sku','other'],true)) Response::error('bad_request','code or code_type is invalid.',400);
        if ($type==='gtin' && !catalog_valid_gtin($code)) Response::error('bad_request','GTIN must have a supported length and valid check digit.',400);
        $db=Sor::db(); if (Inv::row('SELECT id FROM product_types WHERE id=:id AND deleted_at IS NULL',[':id'=>$typeId])===null) Response::error('not_found',"No product type $typeId",404);
        try { $db->beginTransaction(); $source=Sor::dashboardSourceId(); $id=catalog_insert_barcode($db,['code'=>$code,'code_type'=>$type],$typeId,$source); $db->commit(); Response::ok(['id'=>$id,'product_type_id'=>$typeId,'code'=>$code,'code_type'=>$type],201); }
        catch(PDOException $e){if($db->inTransaction())$db->rollBack(); if(($e->errorInfo[1]??null)===1062)Response::error('duplicate_barcode','That barcode is already attached to a live product type.',409);error_log('product barcode create failed: '.$e->getMessage());Response::error('service_unavailable','Inventory storage is unavailable.',503);}
    }, $operator);

    $router->add('DELETE','/api/inventory/product-barcodes/{id}',static function(array $p):void {
        $id=Inv::idParam($p['id']); $db=Sor::db(); $row=Inv::row('SELECT id,product_type_id,code,code_type FROM product_barcodes WHERE id=:id AND deleted_at IS NULL',[':id'=>$id]);
        if($row===null)Response::error('not_found',"No product barcode $id",404);
        $source=Sor::dashboardSourceId(); $db->beginTransaction();
        try{$db->prepare("UPDATE product_barcodes SET deleted_at=NOW(),source_id=:source,asserted_kind='human',asserted_at=NOW() WHERE id=:id AND deleted_at IS NULL")->execute([':source'=>$source,':id'=>$id]);catalog_audit($db,'product_barcodes',$id,'soft_delete',$row,['deleted_at'=>'now'],$source);$db->commit();Response::ok(['id'=>$id,'status'=>'deleted']);}
        catch(Throwable $e){if($db->inTransaction())$db->rollBack();throw $e;}
    },$operator);
};

function catalog_page_int($raw, int $min, int $max): int { if(!is_string($raw)&&!is_int($raw))Response::error('bad_request','Pagination values must be integers.',400);$s=(string)$raw;if(!preg_match('/\A(?:0|[1-9][0-9]*)\z/',$s)||(float)$s>$max||(int)$s<$min)Response::error('bad_request','Pagination value is out of range.',400);return (int)$s; }
function catalog_normalize_code(string $code): string { return strtoupper(preg_replace('/\s+/u','',trim($code)) ?? ''); }
function catalog_valid_gtin(string $code): bool { if(!preg_match('/\A(?:\d{8}|\d{12}|\d{13}|\d{14})\z/',$code))return false;$sum=0;$digits=str_split($code);$check=(int)array_pop($digits);$weight=3;for($i=count($digits)-1;$i>=0;$i--){$sum+=(int)$digits[$i]*$weight;$weight=$weight===3?1:3;}return ((10-$sum%10)%10)===$check; }
function catalog_validate_type(array $body, bool $partial): array {
    $allowed=['kind','name','brand','model','category','specs','notes','image_url'];$unknown=array_diff(array_keys($body),$allowed);if($unknown)Response::error('bad_request','Unsupported product type field: '.reset($unknown),400);
    $out=[];$lengths=['name'=>160,'brand'=>128,'model'=>128,'category'=>128,'notes'=>65535,'image_url'=>1024];
    if(!$partial||array_key_exists('name',$body)){if(!is_string($body['name']??null)||trim($body['name'])==='')Response::error('bad_request','name is required.',400);$out[':name']=trim($body['name']);}
    if(!$partial||array_key_exists('kind',$body)){$kind=$body['kind']??'hardware';if(!is_string($kind)||!in_array($kind,['hardware','peripheral','component','software','accessory','consumable','other'],true))Response::error('bad_request','kind is invalid.',400);$out[':kind']=$kind;}
    foreach(['brand','model','category','notes','image_url'] as $field)if(!$partial||array_key_exists($field,$body)){$v=$body[$field]??null;if($v!==null&&!is_string($v))Response::error('bad_request',"$field must be a string or null.",400);$v=is_string($v)?trim($v):null;if($v===''||$v===null)$v=null;if($v!==null&&strlen($v)>$lengths[$field])Response::error('bad_request',"$field is too long.",400);if($field==='image_url'&&$v!==null&&!preg_match('#\Ahttps?://[^\s]+\z#i',$v))Response::error('bad_request','image_url must be an http(s) URL.',400);$out[":$field"]=$v;}
    if(!$partial||array_key_exists('specs',$body)){$spec=$body['specs']??null;if($spec!==null&&!is_array($spec))Response::error('bad_request','specs must be a JSON object or null.',400);if(is_array($spec)&&array_is_list($spec)&&$spec!==[])Response::error('bad_request','specs must be a JSON object.',400);if(catalog_has_secret_key($spec))Response::error('bad_request','Secret-like licence fields are not accepted.',400);$out[':specs']=$spec===null?null:json_encode($spec===[]?(object)[]:$spec,JSON_THROW_ON_ERROR|JSON_UNESCAPED_SLASHES);}
    return $out;
}
function catalog_has_secret_key($value): bool { if(!is_array($value))return false;foreach($value as $k=>$v){if(is_string($k)&&preg_match('/(?:secret|password|passwd|token|api.?key|license.?key|licence.?key|credential|private.?key)/i',$k))return true;if(catalog_has_secret_key($v))return true;}return false; }
function catalog_barcode_input(array $body): ?array { if(!array_key_exists('barcode',$body))return null;if(!is_array($body['barcode']))Response::error('bad_request','barcode must be an object.',400);$code=catalog_normalize_code(is_string($body['barcode']['code']??null)?$body['barcode']['code']:'');$type=$body['barcode']['code_type']??'other';if($code===''||strlen($code)>128||!in_array($type,['gtin','sku','other'],true))Response::error('bad_request','barcode is invalid.',400);if($type==='gtin'&&!catalog_valid_gtin($code))Response::error('bad_request','GTIN check digit is invalid.',400);return ['code'=>$code,'code_type'=>$type]; }
function catalog_insert_barcode(PDO $db,array $barcode,int $typeId,int $source): int { $db->prepare("INSERT INTO product_barcodes(product_type_id,code,code_type,source_id,asserted_kind,asserted_at) VALUES(:type,:code,:kind,:source,'human',NOW())")->execute([':type'=>$typeId,':code'=>$barcode['code'],':kind'=>$barcode['code_type'],':source'=>$source]);$id=(int)$db->lastInsertId();catalog_audit($db,'product_barcodes',$id,'insert',null,['product_type_id'=>$typeId,'code'=>$barcode['code'],'code_type'=>$barcode['code_type']],$source);return $id; }
function catalog_audit_values(array $values): array { $out=[];foreach($values as $key=>$value){$column=ltrim((string)$key,':');$out[$column]=$column==='specs'&&is_string($value)?json_decode($value,true):$value;}return $out; }
function catalog_audit(PDO $db,string $table,int $id,string $action,?array $old,?array $new,int $source): void { $st=$db->prepare('INSERT INTO audit_log(table_name,row_id,action,actor_label,source_id,asserted_kind,old_values,new_values,note) VALUES(:table,:id,:action,:actor,:source,\'human\',:old,:new,:note)');$st->execute([':table'=>$table,':id'=>$id,':action'=>$action,':actor'=>'dashboard:'.Operator::name(),':source'=>$source,':old'=>$old===null?null:json_encode($old,JSON_THROW_ON_ERROR),':new'=>$new===null?null:json_encode($new,JSON_THROW_ON_ERROR),':note'=>'Inventory catalog API']); }
function catalog_get_type(int $id): array { return catalog_type_row(Inv::row('SELECT id,kind,name,brand,model,category,specs,notes,image_url,lookup_source,lookup_ref,created_at,updated_at FROM product_types WHERE id=:id',[':id'=>$id])); }
function catalog_type_row(array $r): array { if(isset($r['id']))$r['id']=(int)$r['id'];if(isset($r['specs'])&&is_string($r['specs']))$r['specs']=json_decode($r['specs'],true);return $r; }
function catalog_legacy_hw_type(string $kind, string $current): string { if($kind==='hardware'){if(!in_array($current,['printer','scanner','other'],true))return $current;foreach(Inv::enumValues('hardware_models','hw_type') as $candidate)if(!in_array($candidate,['printer','scanner','other'],true))return $candidate;return 'other';}if($kind==='peripheral')return in_array($current,['printer','scanner'],true)?$current:'printer';return 'other'; }
