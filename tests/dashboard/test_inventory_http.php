<?php
declare(strict_types=1);

/* Exercise authenticated inventory HTTP contracts against loopback scratch SoR. */
if (getenv('SOLARI_TEST_SOR_DB') !== '1') {
    fwrite(STDERR, "SOLARI_TEST_SOR_DB=1 is required\n");
    exit(2);
}
$host = getenv('SOR_DB_HOST') ?: '';
$port = getenv('SOR_DB_PORT') ?: '3306';
$name = getenv('SOR_DB_NAME') ?: '';
if (!in_array($host, ['127.0.0.1', 'localhost'], true)
    || !preg_match('/^solarinet_[a-z0-9_]+$/', $name) || !ctype_digit($port)) {
    fwrite(STDERR, "Refusing to use a non-local or non-scratch SoR database\n");
    exit(2);
}

function check(bool $condition, string $message): void
{
    $GLOBALS['inventory_http_assertions'] = ($GLOBALS['inventory_http_assertions'] ?? 0) + 1;
    if (!$condition) throw new RuntimeException($message);
}

function httpRequest(string $url, string $method, ?array $body = null,
                     ?string $cookie = null, ?string $csrf = null): array
{
    $host = (string) parse_url($url, PHP_URL_HOST);
    $port = (int) parse_url($url, PHP_URL_PORT);
    $origin = "http://{$host}:{$port}";
    $headers = ['Accept: application/json', 'Origin: ' . $origin];
    $encodedBody = $body === null ? '' : json_encode($body, JSON_THROW_ON_ERROR);
    if ($body !== null) {
        $headers[] = 'Content-Type: application/json';
        // PHP's stream wrapper may use chunked transfer for large content.
        // The built-in PHP test server does not consistently expose that body
        // through php://input, so send an explicit length for deterministic
        // multi-megabyte API contract tests.
        $headers[] = 'Content-Length: ' . strlen($encodedBody);
    }
    if ($cookie !== null) $headers[] = 'Cookie: ' . $cookie;
    if ($csrf !== null) $headers[] = 'X-Solari-CSRF: ' . $csrf;
    $options = ['http' => [
        'method' => $method,
        'header' => implode("\r\n", $headers),
        'content' => $encodedBody,
        'ignore_errors' => true,
        'timeout' => 5,
    ]];
    $context = stream_context_create($options);
    $response = file_get_contents($url, false, $context);
    if (!is_string($response)) throw new RuntimeException('HTTP fixture request failed.');
    $status = 0;
    $setCookie = null;
    foreach ($http_response_header ?? [] as $header) {
        if (preg_match('/^HTTP\/\S+\s+(\d{3})/', $header, $m)) $status = (int) $m[1];
        if (stripos($header, 'Set-Cookie:') === 0
            && preg_match('/^Set-Cookie:\s*([^;]+)/i', $header, $m)) $setCookie = $m[1];
    }
    $decoded = json_decode($response, true);
    return ['status' => $status, 'body' => is_array($decoded) ? $decoded : [], 'cookie' => $setCookie];
}

function fixtureSession(string $baseUrl, string $role): array
{
    $result = httpRequest($baseUrl . '/__fixture_session?role=' . rawurlencode($role), 'GET');
    check($result['status'] === 200 && is_string($result['cookie']), 'test session fixture did not establish a cookie');
    $csrf = $result['body']['csrf'] ?? null;
    check(is_string($csrf) && preg_match('/\A[a-f0-9]{64}\z/', $csrf) === 1, 'test session fixture did not return a CSRF token');
    return [$result['cookie'], $csrf];
}

$db = new PDO("mysql:host=$host;port=$port;dbname=$name;charset=utf8mb4", getenv('SOR_DB_USER') ?: '', getenv('SOR_DB_PASS') ?: '', [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION, PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC]);
$suffix = bin2hex(random_bytes(6)); $make = 'HTTP fixture '.$suffix; $model = 'RackWire '.$suffix;
$tag = 'HTTP-'.$suffix; $serial = 'SER-'.$suffix; $loc = 'HTTP location '.$suffix; $vendor = 'HTTP receipt '.$suffix; $user = 'inventory-http-test';
$receiveCode = 'F22-' . $suffix; $receiveFailTrigger = 'f22_receive_fail_' . $suffix;
$wp4Prefix = 'WP4-' . $suffix;
$i1cPrefix = 'I1C-' . $suffix;
$ids = ['location'=>null,'model'=>null,'unit'=>null,'barcode'=>null,'receipt'=>null,'entity'=>null,'conflict'=>null,'receivedUnit'=>null,'catalogType'=>null,'catalogBarcode'=>null,'catalogUnit'=>null];
$work = sys_get_temp_dir().'/solarinet-inventory-http-'.bin2hex(random_bytes(8)); $sessions=$work.'/sessions'; $server=null; $failure=null;
$cleanup = static function () use ($db,&$ids,$tag,$receiveCode,$receiveFailTrigger,$vendor,$make,$model,$loc,$wp4Prefix,$i1cPrefix): void {
    try { $db->exec('DROP TABLE IF EXISTS wp4_unknown_location_refs'); } catch (Throwable $e) {}
    try {
        $like=$db->quote($wp4Prefix.'%');
        $db->exec("DELETE FROM barcode_scans WHERE subject_table='locations' AND subject_id IN (SELECT id FROM locations WHERE name LIKE $like)");
        $db->exec("DELETE FROM external_refs WHERE subject_table='locations' AND subject_id IN (SELECT id FROM locations WHERE name LIKE $like)");
        $db->exec("DELETE FROM barcodes WHERE subject_table='locations' AND subject_id IN (SELECT id FROM locations WHERE name LIKE $like)");
        $db->exec("DELETE FROM audit_log WHERE (table_name='locations' AND row_id IN (SELECT id FROM locations WHERE name LIKE $like)) OR (table_name='container_types' AND row_id IN (SELECT id FROM container_types WHERE name LIKE $like))");
        $db->exec("UPDATE locations SET container_type_id=NULL WHERE name LIKE $like");
        $db->exec("DELETE FROM entities WHERE name LIKE $like");
        $db->exec("DELETE FROM power_circuits WHERE name LIKE $like");
        $leaf=$db->prepare('SELECT l.id FROM locations l WHERE l.name LIKE ? AND NOT EXISTS (SELECT 1 FROM locations c WHERE c.parent_id=l.id) ORDER BY l.id DESC');
        $remove=$db->prepare('DELETE FROM locations WHERE id=?');
        do { $leaf->execute([$wp4Prefix.'%']);$leafIds=$leaf->fetchAll(PDO::FETCH_COLUMN);foreach($leafIds as $leafId)$remove->execute([(int)$leafId]); } while($leafIds!==[]);
        $db->exec("DELETE FROM container_types WHERE name LIKE $like");
        $db->exec("DELETE FROM projects WHERE name LIKE $like");
        $db->exec("DELETE FROM hardware_units WHERE serial LIKE $like");
    } catch (Throwable $e) {}
    try {
        $i1cLike=$db->quote($i1cPrefix.'%');
        $db->exec("DELETE FROM hardware_units WHERE notes LIKE $i1cLike AND installed_on_unit_id IS NOT NULL");
        $db->exec("DELETE FROM hardware_units WHERE notes LIKE $i1cLike");
        $db->exec("DELETE FROM audit_log WHERE table_name='product_types' AND row_id IN (SELECT id FROM product_types WHERE name LIKE $i1cLike)");
        $db->exec("DELETE FROM product_types WHERE name LIKE $i1cLike");
    } catch (Throwable $e) {}
    try { $db->exec("DROP TRIGGER IF EXISTS `$receiveFailTrigger`"); } catch (Throwable $e) {}
    foreach (['barcode_scans'=>"code IN (".$db->quote($tag).','.$db->quote($receiveCode).')','receipts'=>"vendor=".$db->quote($vendor)] as $table=>$where) { try {$db->exec("DELETE FROM $table WHERE $where");} catch (Throwable $e) {} }
    if ($ids['receivedUnit']) { try {$db->prepare('DELETE FROM barcodes WHERE subject_table=\'hardware_units\' AND subject_id=?')->execute([$ids['receivedUnit']]);$db->prepare('DELETE FROM hardware_units WHERE id=?')->execute([$ids['receivedUnit']]);} catch (Throwable $e) {} }
    if ($ids['catalogUnit']) { try {$db->prepare('DELETE FROM hardware_units WHERE id=?')->execute([$ids['catalogUnit']]);} catch(Throwable $e){} }
    foreach (['catalogBarcode'=>'product_barcodes','catalogType'=>'product_types'] as $key=>$table) if ($ids[$key]) { try {$db->prepare('DELETE FROM audit_log WHERE table_name=? AND row_id=?')->execute([$table,$ids[$key]]);$db->prepare("DELETE FROM $table WHERE id=?")->execute([$ids[$key]]);} catch(Throwable $e){} }
    foreach ([['entity_hardware_unit_links','hardware_unit_id',$ids['unit']], ['entities','id',$ids['entity']], ['entities','id',$ids['conflict']], ['barcodes','id',$ids['barcode']], ['hardware_units','id',$ids['unit']], ['hardware_models','id',$ids['model']], ['locations','id',$ids['location']]] as [$table,$column,$id]) { if ($id) { try {$q=$db->prepare("DELETE FROM $table WHERE $column=?");$q->execute([$id]);}catch(Throwable $e){} } }
    try { if ($ids['model']) $db->prepare('DELETE FROM rw_model_groups WHERE hardware_model_id=?')->execute([$ids['model']]); if ($ids['unit']) $db->prepare("DELETE FROM audit_log WHERE (table_name='hardware_units' AND row_id=?) OR (table_name='hardware_models' AND row_id=?) OR (table_name='entities' AND row_id IN (?,?))")->execute([$ids['unit'],$ids['model'],$ids['entity'] ?: 0,$ids['conflict'] ?: 0]); $db->prepare('DELETE FROM receipts WHERE vendor=?')->execute([$vendor]);$db->prepare('DELETE FROM hardware_units WHERE asset_tag=?')->execute([$tag]);$db->prepare('DELETE FROM hardware_models WHERE make=? AND model=?')->execute([$make,$model]);$db->prepare('DELETE FROM locations WHERE name=?')->execute([$loc]);}catch(Throwable $e){}
};
try {
    $marker=$db->query("SELECT marker FROM _solarinet_test_scratch WHERE marker='solarinet-ci-scratch-v1'")->fetchColumn(); check($marker==='solarinet-ci-scratch-v1','disposable scratch SoR marker is missing');
    check(getenv('SOLARI_TEST_SOR_DB') === '1' && in_array($host,['127.0.0.1','localhost'],true) && preg_match('/^solarinet_[a-z0-9_]+$/',$name) && ctype_digit($port), 'scratch SoR guard failed');
    $manual=(int)$db->query("SELECT id FROM sources WHERE slug='manual'")->fetchColumn(); check($manual>0,'manual source missing');
    $db->prepare("INSERT INTO locations(name,loc_type,source_id,asserted_kind,asserted_at) VALUES(?,'property',?,'human',NOW())")->execute([$loc,$manual]); $ids['location']=(int)$db->lastInsertId();
    $db->prepare("INSERT INTO hardware_models(make,model,hw_type,notes,source_id,asserted_kind,asserted_at) VALUES(?,?,'switch','HTTP fixture',?,'human',NOW())")->execute([$make,$model,$manual]); $ids['model']=(int)$db->lastInsertId();
    $db->prepare("INSERT INTO hardware_units(model_id,serial,asset_tag,location_id,source_id,asserted_kind,asserted_at) VALUES(?,?,?,?,?,'human',NOW())")->execute([$ids['model'],$serial,$tag,$ids['location'],$manual]); $ids['unit']=(int)$db->lastInsertId();
    $db->prepare("INSERT INTO barcodes(code,code_type,symbology,subject_table,subject_id,source_id,asserted_kind,asserted_at) VALUES(?,'asset_tag','code128','hardware_units',?,?,'human',NOW())")->execute([$tag,$ids['unit'],$manual]); $ids['barcode']=(int)$db->lastInsertId();
    $db->prepare("INSERT INTO receipts(vendor,currency) VALUES(?,'USD')")->execute([$vendor]); $ids['receipt']=(int)$db->lastInsertId();
    if (!mkdir($work,0700)||!mkdir($sessions,0700)) throw new RuntimeException('Cannot create private HTTP fixture dirs');
    $api=var_export(__DIR__.'/../../dashboard/api/index.php',true); $auth=var_export(__DIR__.'/../../dashboard/api/lib/Auth.php',true);
    $router = "<?php\n"
        . "\$path = parse_url(\$_SERVER['REQUEST_URI'] ?? '/', PHP_URL_PATH);\n"
        . "if (\$path === '/__fixture_session') { require_once $auth; \n"
        . " \$role = (\$_GET['role'] ?? 'operator') === 'viewer' ? 'viewer' : 'operator';\n"
        . " Auth::establishSession(['username'=>'inventory-http-test','role'=>\$role,'source'=>'directory']);\n"
        . " header('Content-Type: application/json'); echo json_encode(['csrf'=>Auth::csrfToken()]); exit; }\n"
        . "if (is_string(\$path) && str_starts_with(\$path, '/api/')) { require $api; exit; }\nhttp_response_code(404);";
    file_put_contents($work.'/router.php',$router);
    $sock=stream_socket_server('tcp://127.0.0.1:0',$errno,$errstr); if(!$sock) throw new RuntimeException('Cannot reserve loopback port'); $addr=stream_socket_get_name($sock,false);fclose($sock);$portNo=(int)substr(strrchr($addr,':'),1);
    $server=proc_open([PHP_BINARY,'-d','session.save_path='.$sessions,'-S','127.0.0.1:'.$portNo,$work.'/router.php'],[0=>['file','/dev/null','r'],1=>['file','/dev/null','a'],2=>['file','/dev/null','a']],$pipes);
    $base='http://127.0.0.1:'.$portNo; $ready=false; $until=microtime(true)+10; while(microtime(true)<$until){$probe=@fsockopen('127.0.0.1',$portNo,$e,$es,.1);if(is_resource($probe)){fclose($probe);$ready=true;break;}usleep(100000);} check($ready,'local PHP server did not become ready in 10 seconds');
    [$viewerCookie,$viewerCsrf]=fixtureSession($base,'viewer'); [$cookie,$csrf]=fixtureSession($base,'operator');
    $old=httpRequest($base.'/api/inventory/scanner-import/preview','POST',[], $cookie,$csrf); check($old['status']===404,'valid operator scanner-import route must be retired (404)');
    $path='/api/inventory/components/'.$ids['unit'].'/entity-link';
    $entityName='http-entity-'.$suffix; $db->prepare("INSERT INTO entities(name,entity_type,source_id,asserted_kind) VALUES(?,'network_gear',?,'human')")->execute([$entityName,$manual]);$ids['entity']=(int)$db->lastInsertId();
    $preview=httpRequest($base.$path,'GET',null,$cookie); check($preview['status']===200 && ($preview['body']['data']['entity']??null)===null && in_array($ids['entity'],array_column($preview['body']['data']['candidates']??[],'id'),true),'entity-link candidate preview failed');
    foreach ([['POST',$viewerCookie,$viewerCsrf],['POST',$cookie,null],['DELETE',$viewerCookie,$viewerCsrf],['DELETE',$cookie,null]] as [$method,$c,$t]) check(httpRequest($base.$path,$method,['entity_id'=>$ids['entity']],$c,$t)['status']===403,'entity link/unlink authorization or CSRF rejection failed');
    $linked=httpRequest($base.$path,'POST',['entity_id'=>$ids['entity']],$cookie,$csrf); check($linked['status']===200,'entity link failed');
    $components=httpRequest($base.'/api/inventory/components?limit=1000&offset=0','GET',null,$cookie); $row=null;foreach($components['body']['data']['items']??[] as $x)if((int)$x['id']===$ids['unit'])$row=$x;check(($row['entityName']??null)===$entityName,'components view did not show linked entity');
    $repeat=httpRequest($base.$path,'POST',['entity_id'=>$ids['entity']],$cookie,$csrf);check(($repeat['body']['data']['unchanged']??false)===true,'repeat link was not unchanged');
    $db->prepare("INSERT INTO entities(name,entity_type,source_id,asserted_kind) VALUES(?,'network_gear',?,'human')")->execute(['http-conflict-'.$suffix,$manual]);$ids['conflict']=(int)$db->lastInsertId();$conflict=httpRequest($base.$path,'POST',['entity_id'=>$ids['conflict']],$cookie,$csrf);check($conflict['status']===409&&($conflict['body']['error']['code']??null)==='unit_already_linked','conflicting entity link did not return 409');
    $unlinked=httpRequest($base.$path,'DELETE',['note'=>'HTTP test'],$cookie,$csrf);check($unlinked['status']===200,'entity unlink failed');
    $profile=[['k'=>'ethernet','label'=>'Ethernet x8','glyph'=>'rj45','count'=>8,'rows'=>1,'role'=>'lan','gbps'=>1,'domain'=>'net','conn'=>'rj45']];$save=httpRequest($base.'/api/rackwire/library/'.$ids['model'].'/groups','POST',['name'=>$make.' '.$model,'kind'=>'switch','drawW'=>18,'groups'=>$profile],$cookie,$csrf);check($save['status']===201,'RackWire profile save failed');$library=httpRequest($base.'/api/rackwire/library','GET',null,$cookie);$found=null;foreach($library['body']['data']??[] as $r)if((int)($r['id']??0)===$ids['model'])$found=$r;check(($found['groups'][0]['conn']??null)==='rj45'&&($found['drawW']??null)===18,'RackWire profile readback failed');
    $look=httpRequest($base.'/api/inventory/codes/lookup','POST',['code'=>$tag],$cookie,$csrf);check($look['status']===200&&($look['body']['data']['found']??false)===true&&($look['body']['data']['subject']['id']??null)===$ids['unit'],'barcode resolution failed');
    $countStmt=$db->prepare("SELECT COUNT(*) FROM barcodes WHERE subject_table='hardware_units' AND subject_id=? AND code_type='asset_tag' AND deleted_at IS NULL");$countStmt->execute([$ids['unit']]);$barcodeCount=(int)$countStmt->fetchColumn();
    $autoBody=['code_type'=>'asset_tag','subject_table'=>'hardware_units','subject_id'=>$ids['unit']];
    $autoFirst=httpRequest($base.'/api/inventory/codes','POST',$autoBody,$cookie,$csrf);
    $autoRetry=httpRequest($base.'/api/inventory/codes','POST',$autoBody,$cookie,$csrf);
    check($autoFirst['status']===201&&$autoRetry['status']===201&&($autoFirst['body']['data']['id']??null)===($autoRetry['body']['data']['id']??null),'auto-generated barcode retry did not reuse its subject/type natural key');
    $countStmt->execute([$ids['unit']]);
    check((int)$countStmt->fetchColumn()===$barcodeCount,'auto-generated barcode retry created a duplicate label');
    $db->exec("CREATE TRIGGER `$receiveFailTrigger` BEFORE INSERT ON barcodes FOR EACH ROW BEGIN IF NEW.code = ".$db->quote($receiveCode)." THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'injected F-22 receive failure'; END IF; END");
    $outboxBefore = (int)$db->query('SELECT COALESCE(MAX(id),0) FROM sor_outbox')->fetchColumn();
    $barcodesBefore = (int)$db->query('SELECT COUNT(*) FROM barcodes')->fetchColumn();
    $scansBefore = (int)$db->query('SELECT COUNT(*) FROM barcode_scans')->fetchColumn();
    $receiveBody=['code'=>$receiveCode,'code_type'=>'serial','model_id'=>$ids['model']];
    $failedReceive=httpRequest($base.'/api/inventory/codes/receive','POST',$receiveBody,$cookie,$csrf);check($failedReceive['status']>=500,'injected barcode receive failure unexpectedly succeeded');
    check((int)$db->query('SELECT COUNT(*) FROM hardware_units WHERE serial='.$db->quote($receiveCode))->fetchColumn()===0,'failed barcode receive left a hardware unit');
    check((int)$db->query('SELECT COUNT(*) FROM barcodes')->fetchColumn()===$barcodesBefore,'failed barcode receive left a partial barcode row');
    check((int)$db->query('SELECT COUNT(*) FROM barcode_scans')->fetchColumn()===$scansBefore,'failed barcode receive left a partial scan audit row');
    check((int)$db->query('SELECT COUNT(*) FROM sor_outbox WHERE id > '.$outboxBefore)->fetchColumn()===0,'failed barcode receive emitted an outbox event');
    $db->exec("DROP TRIGGER `$receiveFailTrigger`");
    $received=httpRequest($base.'/api/inventory/codes/receive','POST',$receiveBody,$cookie,$csrf);check($received['status']===200||$received['status']===201,'retry of barcode receive failed');
    $ids['receivedUnit']=(int)($received['body']['data']['unit']['id']??0);
    $replayed=httpRequest($base.'/api/inventory/codes/receive','POST',$receiveBody,$cookie,$csrf);check($replayed['status']===409,'replayed scan did not resolve through the scanned-code natural key');
    check($ids['receivedUnit']>0&&(int)$db->query('SELECT COUNT(*) FROM hardware_units WHERE serial='.$db->quote($receiveCode))->fetchColumn()===1,'receive retry did not create exactly one hardware unit');
    check((int)$db->query('SELECT COUNT(*) FROM barcodes WHERE code='.$db->quote($receiveCode))->fetchColumn()===1,'receive retry did not create exactly one scanned barcode');
    check((int)$db->query('SELECT COUNT(*) FROM barcode_scans WHERE code='.$db->quote($receiveCode).' AND action=\'receive\'')->fetchColumn()===1,'receive retry did not create exactly one scan audit row');
    foreach(['/api/inventory/models','/api/inventory/components','/api/inventory/receipts','/api/inventory/projects','/api/inventory/locations','/api/inventory/allocations'] as $p){$r=httpRequest($base.$p.($p==='/api/inventory/locations'?'?flat=1&':'?').'limit=1&offset=0','GET',null,$cookie);check($r['status']===200&&is_array($r['body']['data']['items']??null)&&isset($r['body']['data']['page']['total']),"paged endpoint failed: $p");}
    // Native prepares reject a reused named placeholder (HY093); every ?q= search must bind each once.
    foreach(['/api/inventory/models','/api/inventory/components','/api/inventory/receipts','/api/inventory/projects','/api/inventory/product-types'] as $p){foreach(['','&limit=5&offset=0'] as $pg){$r=httpRequest($base.$p.'?q=zz-no-match'.$pg,'GET',null,$cookie);check($r['status']===200,"q search failed ($r[status]): $p$pg");}}
    check(httpRequest($base.'/api/inventory/models?limit=0','GET',null,$cookie)['status']===400,'limit=0 was accepted');
    $aggregate=httpRequest($base.'/api/inventory','GET',null,$cookie);$d=$aggregate['body']['data']??[];check($aggregate['status']===200&&is_array($d['models']??null)&&is_array($d['units']??null)&&!isset($d['page']),'combined inventory shape changed');
    // 031: operator edit of hardware_models.arch (provisioning's first arch source).
    $mpath='/api/inventory/models/'.$ids['model'];
    foreach ([[$viewerCookie,$viewerCsrf],[$cookie,null]] as [$c,$t]) check(httpRequest($base.$mpath,'PUT',['arch'=>'arm64'],$c,$t)['status']===403,'model PUT authorization or CSRF rejection failed');
    check(httpRequest($base.$mpath,'PUT',['arch'=>'aarch64'],$cookie,$csrf)['status']===400,'model PUT accepted an arch outside the 031 enum');
    check(httpRequest($base.$mpath,'PUT',['arch'=>7],$cookie,$csrf)['status']===400,'model PUT accepted a non-string arch');
    check(httpRequest($base.$mpath,'PUT',[],$cookie,$csrf)['status']===400,'model PUT with no fields was accepted');
    check(httpRequest($base.'/api/inventory/models/999999999','PUT',['arch'=>'arm64'],$cookie,$csrf)['status']===404,'model PUT on a missing model was not 404');
    $archStmt=$db->prepare('SELECT arch FROM hardware_models WHERE id=?');
    $archStmt->execute([$ids['model']]); check($archStmt->fetchColumn()===null,'rejected model PUTs changed arch');
    // Arch-only: the hardened grant is column-scoped to arch + provenance.
    foreach ([['arch'=>'arm64','notes'=>'x'],['make'=>'Other'],['hw_type'=>'server']] as $bad) check(httpRequest($base.$mpath,'PUT',$bad,$cookie,$csrf)['status']===400,'model PUT accepted a non-arch field');
    $archStmt->execute([$ids['model']]); check($archStmt->fetchColumn()===null,'rejected non-arch model PUT changed arch');
    $put=httpRequest($base.$mpath,'PUT',['arch'=>'arm64'],$cookie,$csrf);
    check($put['status']===200&&($put['body']['data']['arch']??null)==='arm64','model PUT arch=arm64 failed');
    $prov=$db->prepare('SELECT source_id, asserted_kind FROM hardware_models WHERE id=?'); $prov->execute([$ids['model']]); [$psrc,$pkind]=$prov->fetch(PDO::FETCH_NUM);
    check((int)$psrc!==$manual&&$pkind==='human','model PUT did not re-assert dashboard human provenance');
    $archStmt->execute([$ids['model']]); check($archStmt->fetchColumn()==='arm64','model PUT did not persist arch');
    $one=httpRequest($base.$mpath,'GET',null,$viewerCookie); check(($one['body']['data']['arch']??null)==='arm64','model GET does not expose arch');
    $clr=httpRequest($base.$mpath,'PUT',['arch'=>''],$cookie,$csrf);
    check($clr['status']===200&&array_key_exists('arch',$clr['body']['data']??[])&&$clr['body']['data']['arch']===null,"model PUT arch='' did not clear to NULL");
    $archStmt->execute([$ids['model']]); check($archStmt->fetchColumn()===null,"model PUT arch='' did not store NULL");
    // Product catalog contract: policy, validation, exact lookup, uniqueness, audit, and soft deletes.
    check(httpRequest($base.'/api/inventory/product-types','GET')['status']===401,'catalog read allowed an unauthenticated request');
    foreach ([[$viewerCookie,$viewerCsrf],[$cookie,null]] as [$c,$t]) check(httpRequest($base.'/api/inventory/product-types','POST',['name'=>'Forbidden'], $c,$t)['status']===403,'catalog write did not enforce operator/CSRF policy');
    check(httpRequest($base.'/api/inventory/product-types','POST',['name'=>'Bad','specs'=>['license_key'=>'must-not-store']],$cookie,$csrf)['status']===400,'catalog accepted secret-like specs');
    check(httpRequest($base.'/api/inventory/product-types','POST',['kind'=>'nope','name'=>'Bad'],$cookie,$csrf)['status']===400,'catalog accepted invalid kind');
    $newType=httpRequest($base.'/api/inventory/product-types','POST',['kind'=>'software','name'=>'HTTP catalog '.$suffix,'brand'=>'Example','specs'=>['seats'=>4]],$cookie,$csrf);
    check($newType['status']===201,'product type create failed: '.json_encode($newType['body'])); $ids['catalogType']=(int)$newType['body']['data']['id'];
    $search=httpRequest($base.'/api/inventory/product-types?q='.rawurlencode('catalog '.$suffix).'&kind=software','GET',null,$viewerCookie);
    check($search['status']===200&&array_column($search['body']['data']['items']??[],'id')===[$ids['catalogType']]&&($search['body']['data']['page']['total']??null)===1,'catalog q+kind search failed: '.json_encode($search['body']));
    check((httpRequest($base.'/api/inventory/product-types?q='.rawurlencode('catalog '.$suffix).'&kind=hardware','GET',null,$viewerCookie)['body']['data']['page']['total']??null)===0,'catalog kind filter did not narrow a q search');
    check(httpRequest($base.'/api/inventory/product-types/999999999','GET',null,$cookie)['status']===404,'missing product type did not return 404');
    $catPath='/api/inventory/product-types/'.$ids['catalogType'];
    check(httpRequest($base.$catPath,'PUT',['name'=>'No token '.$suffix],$cookie,$csrf)['status']===400,'product type PUT without updated_at was accepted');
    $updated=httpRequest($base.$catPath,'PUT',['name'=>'Renamed catalog '.$suffix,'updated_at'=>(string)($newType['body']['data']['updated_at']??'')],$cookie,$csrf);check($updated['status']===200&&($updated['body']['data']['name']??'')==='Renamed catalog '.$suffix,'product type rename failed');
    check(httpRequest($base.$catPath,'PUT',['name'=>'stale','updated_at'=>'2000-01-01 00:00:00'],$cookie,$csrf)['status']===409,'stale product type update did not return conflict');
    $typesBeforeLookup=(int)$db->query('SELECT COUNT(*) FROM product_types')->fetchColumn();
    $missingLookup=httpRequest($base.'/api/inventory/product-barcodes?code=NO-MATCH-'.$suffix,'GET',null,$cookie);check($missingLookup['status']===200&&($missingLookup['body']['data']['found']??true)===false,'unknown product barcode lookup did not return no match');
    check((int)$db->query('SELECT COUNT(*) FROM product_types')->fetchColumn()===$typesBeforeLookup,'lookup unexpectedly created a product type');
    $code='sku-'.$suffix;
    $catBarcode=httpRequest($base.'/api/inventory/product-barcodes','POST',['product_type_id'=>$ids['catalogType'],'code'=>$code,'code_type'=>'sku'],$cookie,$csrf);check($catBarcode['status']===201,'product barcode create failed: '.json_encode($catBarcode['body']));$ids['catalogBarcode']=(int)$catBarcode['body']['data']['id'];
    $listed=httpRequest($base.$catPath,'GET',null,$cookie);check($listed['status']===200&&array_column($listed['body']['data']['barcodes']??[],'id')===[$ids['catalogBarcode']]&&($listed['body']['data']['barcodes'][0]['code_type']??'')==='sku','product type detail did not list its live barcode: '.json_encode($listed['body']['data']['barcodes']??null));
    $dup=httpRequest($base.'/api/inventory/product-barcodes','POST',['product_type_id'=>$ids['catalogType'],'code'=>strtoupper($code),'code_type'=>'sku'],$cookie,$csrf);check($dup['status']===409&&($dup['body']['error']['code']??'')==='duplicate_barcode','duplicate product barcode did not return 409');
    $foundCat=httpRequest($base.'/api/inventory/product-barcodes?code='.rawurlencode(strtoupper($code)),'GET',null,$cookie);check($foundCat['status']===200&&($foundCat['body']['data']['found']??false)===true&&($foundCat['body']['data']['productType']['id']??0)===$ids['catalogType'],'exact product barcode lookup failed');
    $db->prepare("INSERT INTO hardware_units(model_id,product_type_id,source_id,asserted_kind,asserted_at) VALUES(NULL,?,?,'human',NOW())")->execute([$ids['catalogType'],$manual]); $ids['catalogUnit']=(int)$db->lastInsertId();
    check(httpRequest($base.'/api/inventory/product-barcodes/'.$ids['catalogBarcode'],'DELETE',[],$cookie,$csrf)['status']===200,'product barcode soft-delete failed');
    check((httpRequest($base.$catPath,'GET',null,$cookie)['body']['data']['barcodes']??null)===[],'product type detail still listed a soft-deleted barcode');
    check(httpRequest($base.$catPath,'DELETE',[],$cookie,$csrf)['status']===200,'in-use product type soft-delete failed');
    check((int)$db->query('SELECT COUNT(*) FROM product_types WHERE id='.$ids['catalogType'])->fetchColumn()===1,'product type soft-delete removed a referenced row');
    $reuse=httpRequest($base.'/api/inventory/product-types','POST',['name'=>'Reuse probe '.$suffix],$cookie,$csrf);check($reuse['status']===201,'reuse probe create failed');$reuseId=(int)$reuse['body']['data']['id'];
    $db->prepare("INSERT INTO product_barcodes(product_type_id,code,code_type,source_id,asserted_kind) VALUES(?,?,'other',?,'human')")->execute([$reuseId,'RETIRE-'.$suffix,$manual]);
    check(httpRequest($base.'/api/inventory/product-types/'.$reuseId,'DELETE',[],$cookie,$csrf)['status']===200,'reuse probe delete failed');
    $again=httpRequest($base.'/api/inventory/product-barcodes','POST',['product_type_id'=>$reuseId,'code'=>'RETIRE-'.$suffix],$cookie,$csrf);check($again['status']===404,'barcode POST to a retired type was not refused');
    check((int)$db->query("SELECT COUNT(*) FROM product_barcodes WHERE code=".$db->quote('RETIRE-'.$suffix)." AND deleted_at IS NULL")->fetchColumn()===0,'retiring a product type left its barcode live (code could never be reused)');
    $db->prepare("DELETE FROM audit_log WHERE (table_name='product_types' AND row_id=?) OR (table_name='product_barcodes' AND row_id IN (SELECT id FROM product_barcodes WHERE product_type_id=?))")->execute([$reuseId,$reuseId]);$db->prepare('DELETE FROM product_barcodes WHERE product_type_id=?')->execute([$reuseId]);$db->prepare('DELETE FROM product_types WHERE id=?')->execute([$reuseId]);
    check(httpRequest($base.'/api/inventory/product-types','POST',['name'=>'Bad url','image_url'=>'javascript:alert(1)'],$cookie,$csrf)['status']===400,'catalog accepted a non-http image_url');
    // A legacy-projected type: the 033 trigger rewrites name from the model, so an unrelated PUT must not revert an earlier rename.
    $legacyType=(int)$db->query('SELECT product_type_id FROM hardware_models WHERE id='.(int)$ids['model'])->fetchColumn();
    $db->prepare("UPDATE product_types SET lookup_source='legacy:hardware_models' WHERE id=?")->execute([$legacyType]);
    $legacyPath='/api/inventory/product-types/'.$legacyType;
    $typeToken=static fn(string $path):string=>(string)(httpRequest($base.$path,'GET',null,$cookie)['body']['data']['updated_at']??'');
    check(httpRequest($base.$legacyPath,'PUT',['name'=>'Friendly '.$suffix,'kind'=>'component','updated_at'=>$typeToken($legacyPath)],$cookie,$csrf)['status']===200,'legacy type rename failed');
    $notesOnly=httpRequest($base.$legacyPath,'PUT',['notes'=>'notes '.$suffix,'updated_at'=>$typeToken($legacyPath)],$cookie,$csrf);
    check($notesOnly['status']===200&&($notesOnly['body']['data']['name']??'')==='Friendly '.$suffix&&($notesOnly['body']['data']['kind']??'')==='component','notes-only PUT reverted an earlier name/kind edit on a legacy-projected type: '.json_encode($notesOnly['body']['data']??null));
    $db->prepare("DELETE FROM audit_log WHERE table_name='product_types' AND row_id=?")->execute([$legacyType]);

    // WP4 location hierarchy, move/merge, batch, assignment, and container APIs.
    $wp4Loc='/api/inventory/locations';
    $createLoc=static function(array $body)use($base,$cookie,$csrf,$wp4Loc):array{return httpRequest($base.$wp4Loc,'POST',$body,$cookie,$csrf);};
    $rootName=$wp4Prefix.' root';
    $root=$createLoc(['name'=>$rootName,'loc_type'=>'property']);check($root['status']===201,'WP4 property create failed');$wp4Root=(int)$root['body']['data']['id'];
    $dupRoot=$createLoc(['name'=>$rootName,'loc_type'=>'property']);check($dupRoot['status']===409&&($dupRoot['body']['error']['code']??'')==='duplicate_location','duplicate root sibling was not rejected');
    $badRoot=$createLoc(['name'=>$wp4Prefix.' invalid root','loc_type'=>'room']);check($badRoot['status']===400&&($badRoot['body']['error']['code']??'')==='bad_request','invalid root level was not rejected');
    $floor=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' floor','loc_type'=>'floor']);check($floor['status']===201,'WP4 floor create failed');$wp4Floor=(int)$floor['body']['data']['id'];
    $dupChild=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' floor','loc_type'=>'floor']);check($dupChild['status']===409&&($dupChild['body']['error']['code']??'')==='duplicate_location','duplicate child sibling was not rejected');
    $badChild=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' bad child','loc_type'=>'room']);check($badChild['status']===400,'invalid child level was not rejected');
    $rootUpdated=$db->query('SELECT updated_at FROM locations WHERE id='.$wp4Root)->fetchColumn();
    check(httpRequest($base.$wp4Loc.'/'.$wp4Root,'PUT',['name'=>$wp4Prefix.' renamed','updated_at'=>'2000-01-01 00:00:00'],$cookie,$csrf)['status']===409,'stale location updated_at did not conflict');
    $patched=httpRequest($base.$wp4Loc.'/'.$wp4Root,'PUT',['notes'=>'WP4 updated','updated_at'=>$root['body']['data']['updatedAt']],$cookie,$csrf);check($patched['status']===200&&($patched['body']['data']['notes']??null)==='WP4 updated','location PUT did not accept its returned updatedAt value');
    $rootUpdated=(string)($patched['body']['data']['updatedAt']??'');// the edit above made the earlier token stale (037 sub-second lock)
    check(httpRequest($base.$wp4Loc.'/'.$wp4Root,'PUT',['loc_type'=>'room','updated_at'=>$rootUpdated],$cookie,$csrf)['status']===400,'location type change invalidating its child was accepted');
    $cycle=httpRequest($base.$wp4Loc.'/'.$wp4Root.'/move','POST',['parent_id'=>$wp4Floor],$cookie,$csrf);check($cycle['status']===409&&($cycle['body']['error']['code']??'')==='location_cycle','move into descendant did not return location_cycle');
    $otherRoot=$createLoc(['name'=>$wp4Prefix.' other property','loc_type'=>'property']);$otherFloor=$createLoc(['parent_id'=>(int)$otherRoot['body']['data']['id'],'name'=>$wp4Prefix.' floor','loc_type'=>'floor']);
    $moveDup=httpRequest($base.$wp4Loc.'/'.(int)$otherFloor['body']['data']['id'].'/move','POST',['parent_id'=>$wp4Root],$cookie,$csrf);check($moveDup['status']===409&&($moveDup['body']['error']['code']??'')==='duplicate_location','move sibling duplicate did not return duplicate_location');
    $bulkOne=httpRequest($base.$wp4Loc.'/bulk-storage-units','POST',['parent_id'=>$wp4Root,'count'=>1,'prefix'=>$wp4Prefix.' one-','start'=>1],$cookie,$csrf);check($bulkOne['status']===201&&($bulkOne['body']['data']['count']??0)===1,'bulk count=1 failed');
    $bulkMany=httpRequest($base.$wp4Loc.'/bulk-storage-units','POST',['parent_id'=>$wp4Root,'count'=>500,'prefix'=>$wp4Prefix.' batch-','start'=>1],$cookie,$csrf);check($bulkMany['status']===201&&($bulkMany['body']['data']['count']??0)===500,'bulk count=500 failed: '.json_encode($bulkMany['body']));
    check(httpRequest($base.$wp4Loc.'/bulk-storage-units','POST',['parent_id'=>$wp4Root,'count'=>501,'prefix'=>$wp4Prefix.' too-many-','start'=>1],$cookie,$csrf)['status']===400,'bulk count=501 was accepted');
    $atomic=httpRequest($base.$wp4Loc.'/bulk-storage-units','POST',['parent_id'=>$wp4Root,'count'=>2,'names'=>[$wp4Prefix.' atomic',$wp4Prefix.' atomic']],$cookie,$csrf);check($atomic['status']===409&&($atomic['body']['error']['code']??'')==='duplicate_location','bulk duplicate names were not rejected');
    check((int)$db->query("SELECT COUNT(*) FROM locations WHERE name=".$db->quote($wp4Prefix.' atomic')." AND deleted_at IS NULL")->fetchColumn()===0,'bulk validation failure partially inserted rows');

    $projA=httpRequest($base.'/api/inventory/projects','POST',['name'=>$wp4Prefix.' project A'],$cookie,$csrf);$projB=httpRequest($base.'/api/inventory/projects','POST',['name'=>$wp4Prefix.' project B'],$cookie,$csrf);check($projA['status']===201&&$projB['status']===201,'WP4 test projects failed to create');$wp4ProjectA=(int)$projA['body']['data']['id'];$wp4ProjectB=(int)$projB['body']['data']['id'];
    $empty=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' empty','loc_type'=>'storage_unit','assigned_project_id'=>$wp4ProjectA]);check($empty['status']===201,'empty assigned storage unit create failed');$emptyId=(int)$empty['body']['data']['id'];
    $emptyRead=httpRequest($base.$wp4Loc.'/'.$emptyId,'GET',null,$cookie);check($emptyRead['status']===200&&array_key_exists('containerTypeId',$emptyRead['body']['data']??[])&&($emptyRead['body']['data']['assignedProjectId']??null)===$wp4ProjectA,'location GET omitted container/project metadata');
    $release=httpRequest($base.$wp4Loc.'/'.$emptyId.'/project','PUT',['project_id'=>null],$cookie,$csrf);check($release['status']===200&&array_key_exists('project_id',$release['body']['data']??[])&&$release['body']['data']['project_id']===null,'empty container release failed: '.json_encode($release['body']));
    check(httpRequest($base.$wp4Loc.'/'.$emptyId.'/project','PUT',['project_id'=>$wp4ProjectA],$cookie,$csrf)['status']===200,'container reassignment failed');
    $liveChild=$createLoc(['parent_id'=>$emptyId,'name'=>$wp4Prefix.' nested','loc_type'=>'storage_unit']);check($liveChild['status']===201,'nested storage unit create failed');
    $nonemptyRelease=httpRequest($base.$wp4Loc.'/'.$emptyId.'/project','PUT',['project_id'=>null],$cookie,$csrf);check($nonemptyRelease['status']===409&&($nonemptyRelease['body']['error']['code']??'')==='container_not_empty','non-empty child container release was accepted');
    $emptyUpdated=$db->query('SELECT updated_at FROM locations WHERE id='.$emptyId)->fetchColumn();$putRelease=httpRequest($base.$wp4Loc.'/'.$emptyId,'PUT',['assigned_project_id'=>null,'updated_at'=>$emptyUpdated],$cookie,$csrf);check($putRelease['status']===409&&($putRelease['body']['error']['code']??'')==='container_not_empty','location PUT bypassed the container release check');

    $typeCreate=httpRequest($base.'/api/inventory/container-types','POST',['name'=>$wp4Prefix.' type'],$cookie,$csrf);check($typeCreate['status']===201,'container type create failed');$wp4Type=(int)$typeCreate['body']['data']['id'];
    $typedLoc=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' typed','loc_type'=>'storage_unit','container_type_id'=>$wp4Type]);check($typedLoc['status']===201,'storage unit with container type failed');
    $typeRow=$db->query('SELECT updated_at FROM container_types WHERE id='.$wp4Type)->fetch();
    $renamedType=httpRequest($base.'/api/inventory/container-types/'.$wp4Type,'PUT',['name'=>$wp4Prefix.' renamed type','updated_at'=>$typeRow['updated_at']],$cookie,$csrf);check($renamedType['status']===200,'in-use container type rename failed');
    check(httpRequest($base.'/api/inventory/container-types/'.$wp4Type,'PUT',['name'=>$wp4Prefix.' stale type','updated_at'=>'2000-01-01 00:00:00'],$cookie,$csrf)['status']===409,'stale container type rename did not conflict');
    check(httpRequest($base.'/api/inventory/container-types/'.$wp4Type,'DELETE',[],$cookie,$csrf)['status']===200,'in-use container type retirement failed');
    check(httpRequest($base.'/api/inventory/container-types?include_deleted=1','GET',null,$cookie)['status']===200,'container type include_deleted list failed');
    $liveTypes=httpRequest($base.'/api/inventory/container-types','GET',null,$cookie);check($liveTypes['status']===200&&!in_array($wp4Type,array_map(static fn($r)=>(int)$r['id'],$liveTypes['body']['data']??[]),true),'retired container type remained in the default list');

    $notEmptyUnit=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' with-unit','loc_type'=>'storage_unit','assigned_project_id'=>$wp4ProjectA]);check($notEmptyUnit['status']===201,'storage unit for hardware release refusal failed');$notEmptyId=(int)$notEmptyUnit['body']['data']['id'];
    $db->prepare("INSERT INTO hardware_units(model_id,serial,location_id,source_id,asserted_kind,asserted_at) VALUES(?,?,?,?,'human',NOW())")->execute([$ids['model'],$wp4Prefix.' hardware',$notEmptyId,$manual]);
    $unitRelease=httpRequest($base.$wp4Loc.'/'.$notEmptyId.'/project','PUT',['project_id'=>null],$cookie,$csrf);check($unitRelease['status']===409&&($unitRelease['body']['error']['code']??'')==='container_not_empty','container with hardware was released');

    $source=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' merge source','loc_type'=>'storage_unit','assigned_project_id'=>$wp4ProjectA]);
    $target=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' merge target','loc_type'=>'storage_unit','assigned_project_id'=>$wp4ProjectA]);check($source['status']===201&&$target['status']===201,'merge source/target fixture failed');$sourceId=(int)$source['body']['data']['id'];$targetId=(int)$target['body']['data']['id'];
    $mergeChild=$createLoc(['parent_id'=>$sourceId,'name'=>$wp4Prefix.' merge child','loc_type'=>'storage_unit']);check($mergeChild['status']===201,'merge child fixture failed');$mergeChildId=(int)$mergeChild['body']['data']['id'];
    $db->prepare("INSERT INTO hardware_units(model_id,serial,location_id,source_id,asserted_kind,asserted_at) VALUES(?,?,?,?,'human',NOW())")->execute([$ids['model'],$wp4Prefix.' merge hardware',$sourceId,$manual]);
    $db->prepare("INSERT INTO entities(name,entity_type,location_id,source_id,asserted_kind,asserted_at) VALUES(?,'other',?,?,'human',NOW())")->execute([$wp4Prefix.' entity',$sourceId,$manual]);
    $db->prepare("INSERT INTO power_circuits(name,location_id,source_id,asserted_kind,asserted_at) VALUES(?,?,?,'human',NOW())")->execute([$wp4Prefix.' circuit',$sourceId,$manual]);
    $traySet=httpRequest($base.'/api/inventory/projects/'.$wp4ProjectA,'PUT',['tray_location_id'=>$sourceId],$cookie,$csrf);check($traySet['status']===200,'unable to set project tray-location fixture');
    $mergeCode='WP4BC-'.$suffix;
    $db->prepare("INSERT INTO barcodes(code,code_type,symbology,subject_table,subject_id,source_id,asserted_kind,asserted_at) VALUES(?,'location','qr','locations',?,?,'human',NOW())")->execute([$mergeCode,$sourceId,$manual]);$mergeBarcodeId=(int)$db->lastInsertId();
    $db->prepare("INSERT INTO barcode_scans(code,barcode_id,subject_table,subject_id,action,operator,source_id) VALUES(?,?,'locations',?,'lookup',?,?)")->execute([$mergeCode,$mergeBarcodeId,$sourceId,'inventory-http-test',$manual]);
    $db->prepare("INSERT INTO external_refs(source_id,subject_table,subject_id,external_id) VALUES(?,'locations',?,?)")->execute([$manual,$sourceId,$wp4Prefix.' external']);
    $previewPath=$base.$wp4Loc.'/'.$sourceId.'/merge-preview?target_id='.$targetId;
    $preview=httpRequest($previewPath,'GET',null,$cookie);check($preview['status']===200&&is_string($preview['body']['data']['fingerprint']??null),'merge preview fingerprint missing');$staleFingerprint=$preview['body']['data']['fingerprint'];
    $db->prepare("INSERT INTO external_refs(source_id,subject_table,subject_id,external_id) VALUES(?,'locations',?,?)")->execute([$manual,$sourceId,$wp4Prefix.' stale ref']);
    $stale=httpRequest($base.$wp4Loc.'/'.$sourceId.'/merge','POST',['target_id'=>$targetId,'fingerprint'=>$staleFingerprint],$cookie,$csrf);check($stale['status']===409&&($stale['body']['error']['code']??'')==='merge_conflict','stale merge fingerprint was not rejected');

    // Dynamic unknown-FK guard: preview remains available, merge is explicitly refused.
    $db->exec('CREATE TABLE wp4_unknown_location_refs (id INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY, location_id INT UNSIGNED NOT NULL, CONSTRAINT fk_wp4_unknown_location FOREIGN KEY(location_id) REFERENCES locations(id)) ENGINE=InnoDB');
    $db->prepare('INSERT INTO wp4_unknown_location_refs(location_id) VALUES(?)')->execute([$sourceId]);
    $fresh=httpRequest($previewPath,'GET',null,$cookie);$unknown=httpRequest($base.$wp4Loc.'/'.$sourceId.'/merge','POST',['target_id'=>$targetId,'fingerprint'=>$fresh['body']['data']['fingerprint']],$cookie,$csrf);check($unknown['status']===409&&($unknown['body']['error']['code']??'')==='merge_conflict','unknown hard FK did not block merge');
    $db->exec('DROP TABLE wp4_unknown_location_refs');
    $fresh=httpRequest($previewPath,'GET',null,$cookie);$success=httpRequest($base.$wp4Loc.'/'.$sourceId.'/merge','POST',['target_id'=>$targetId,'fingerprint'=>$fresh['body']['data']['fingerprint']],$cookie,$csrf);check($success['status']===200&&($success['body']['data']['merged']??false)===true,'valid merge failed: '.json_encode($success['body']));
    check((int)$db->query('SELECT location_id FROM hardware_units WHERE serial='.$db->quote($wp4Prefix.' merge hardware'))->fetchColumn()===$targetId,'merge did not rewrite hardware FK');
    check((int)$db->query('SELECT location_id FROM entities WHERE name='.$db->quote($wp4Prefix.' entity'))->fetchColumn()===$targetId,'merge did not rewrite entity location FK');
    check((int)$db->query('SELECT location_id FROM power_circuits WHERE name='.$db->quote($wp4Prefix.' circuit'))->fetchColumn()===$targetId,'merge did not rewrite power circuit location FK');
    check((int)$db->query('SELECT tray_location_id FROM projects WHERE id='.$wp4ProjectA)->fetchColumn()===$targetId,'merge did not rewrite project tray-location FK');
    check((int)$db->query('SELECT parent_id FROM locations WHERE id='.$mergeChildId)->fetchColumn()===$targetId,'merge did not move child locations');
    check((int)$db->query('SELECT subject_id FROM barcodes WHERE id='.$mergeBarcodeId)->fetchColumn()===$targetId,'merge did not retarget live location barcode');
    check((int)$db->query("SELECT COUNT(*) FROM barcode_scans WHERE subject_id=$sourceId AND code=".$db->quote($mergeCode))->fetchColumn()===1,'merge changed barcode scan history');
    check((int)$db->query("SELECT COUNT(*) FROM external_refs WHERE subject_table='locations' AND subject_id=$sourceId")->fetchColumn()===2,'merge changed external reference history');
    $mergeAudit=$db->query("SELECT new_values FROM audit_log WHERE table_name='locations' AND row_id=$sourceId AND action='soft_delete' ORDER BY id DESC LIMIT 1")->fetchColumn();check(is_string($mergeAudit)&&((json_decode($mergeAudit,true)['merge_target_id']??0)===$targetId),'merge target missing from source audit');
    $repeat=httpRequest($base.$wp4Loc.'/'.$sourceId.'/merge','POST',['target_id'=>$targetId,'fingerprint'=>$fresh['body']['data']['fingerprint']],$cookie,$csrf);check($repeat['status']===200&&($repeat['body']['data']['idempotent']??false)===true,'repeated completed merge was not deterministic');
    // Lead-review regressions: merged source cannot move; PUT refuses re-parenting; releasing an unassigned non-empty container is a no-op.
    $deadMove=httpRequest($base.$wp4Loc.'/'.$sourceId.'/move','POST',['parent_id'=>$wp4Root],$cookie,$csrf);check($deadMove['status']===404,'move of a soft-deleted location was accepted: '.json_encode($deadMove['body']));
    $targetRow=httpRequest($base.$wp4Loc.'/'.$targetId,'GET',null,$cookie);$reparent=httpRequest($base.$wp4Loc.'/'.$targetId,'PUT',['parent_id'=>null,'name'=>$wp4Prefix.' reparent','updated_at'=>(string)($targetRow['body']['data']['updatedAt']??'')],$cookie,$csrf);check($reparent['status']===400&&str_contains((string)($reparent['body']['error']['message']??''),'/move'),'PUT parent_id was not refused with a move hint');
    $plain=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' unassigned box','loc_type'=>'storage_unit']);$plainId=(int)$plain['body']['data']['id'];check($createLoc(['parent_id'=>$plainId,'name'=>$wp4Prefix.' unassigned inner','loc_type'=>'storage_unit'])['status']===201,'unassigned container child fixture failed');$noopRelease=httpRequest($base.$wp4Loc.'/'.$plainId.'/project','PUT',['project_id'=>null],$cookie,$csrf);check($noopRelease['status']===200,'release of an unassigned non-empty container returned '.$noopRelease['status']);
    $deepSource=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' deep source','loc_type'=>'storage_unit']);$deepTarget=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' deep target','loc_type'=>'storage_unit']);$dsid=(int)$deepSource['body']['data']['id'];$dtid=(int)$deepTarget['body']['data']['id'];$deepChild=$createLoc(['parent_id'=>$dsid,'name'=>$wp4Prefix.' deep child','loc_type'=>'storage_unit']);$deepGrand=$createLoc(['parent_id'=>(int)$deepChild['body']['data']['id'],'name'=>$wp4Prefix.' deep grandchild','loc_type'=>'storage_unit']);check($deepGrand['status']===201,'deep merge fixture failed');$dp=httpRequest($base.$wp4Loc.'/'.$dsid.'/merge-preview?target_id='.$dtid,'GET',null,$cookie);$dm=httpRequest($base.$wp4Loc.'/'.$dsid.'/merge','POST',['target_id'=>$dtid,'fingerprint'=>$dp['body']['data']['fingerprint']],$cookie,$csrf);check($dm['status']===200&&(int)$db->query('SELECT parent_id FROM locations WHERE id='.(int)$deepChild['body']['data']['id'])->fetchColumn()===$dtid,'merge of a source with grandchildren failed: '.json_encode($dm['body']));
    $longBulk=httpRequest($base.$wp4Loc.'/bulk-storage-units','POST',['parent_id'=>$wp4Root,'count'=>2,'prefix'=>str_repeat('x',100).str_repeat('y',27),'start'=>10],$cookie,$csrf);check($longBulk['status']===400,'over-length bulk prefix returned '.$longBulk['status']);

    $different=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' other level','loc_type'=>'floor']);$diffId=(int)$different['body']['data']['id'];$diffPreview=httpRequest($base.$wp4Loc.'/'.$emptyId.'/merge-preview?target_id='.$diffId,'GET',null,$cookie);$diffMerge=httpRequest($base.$wp4Loc.'/'.$emptyId.'/merge','POST',['target_id'=>$diffId,'fingerprint'=>$diffPreview['body']['data']['fingerprint']],$cookie,$csrf);check($diffMerge['status']===409&&($diffMerge['body']['error']['code']??'')==='merge_conflict','different-level merge was not refused');
    $collideSource=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' collision source','loc_type'=>'storage_unit']);$collideTarget=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' collision target','loc_type'=>'storage_unit']);
    $createLoc(['parent_id'=>(int)$collideSource['body']['data']['id'],'name'=>$wp4Prefix.' same child','loc_type'=>'storage_unit']);$createLoc(['parent_id'=>(int)$collideTarget['body']['data']['id'],'name'=>$wp4Prefix.' same child','loc_type'=>'storage_unit']);
    $csid=(int)$collideSource['body']['data']['id'];$ctid=(int)$collideTarget['body']['data']['id'];$cp=httpRequest($base.$wp4Loc.'/'.$csid.'/merge-preview?target_id='.$ctid,'GET',null,$cookie);$cm=httpRequest($base.$wp4Loc.'/'.$csid.'/merge','POST',['target_id'=>$ctid,'fingerprint'=>$cp['body']['data']['fingerprint']],$cookie,$csrf);check($cm['status']===409&&($cm['body']['error']['code']??'')==='merge_conflict','child name collision merge was accepted: '.json_encode($cm['body']));
    $cycleSource=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' cycle source','loc_type'=>'storage_unit']);$cycleSourceId=(int)$cycleSource['body']['data']['id'];$cycleTarget=$createLoc(['parent_id'=>$cycleSourceId,'name'=>$wp4Prefix.' cycle target','loc_type'=>'storage_unit']);$cycleTargetId=(int)$cycleTarget['body']['data']['id'];$cyclePreview=httpRequest($base.$wp4Loc.'/'.$cycleSourceId.'/merge-preview?target_id='.$cycleTargetId,'GET',null,$cookie);$cycleMerge=httpRequest($base.$wp4Loc.'/'.$cycleSourceId.'/merge','POST',['target_id'=>$cycleTargetId,'fingerprint'=>$cyclePreview['body']['data']['fingerprint']],$cookie,$csrf);check($cycleMerge['status']===409&&($cycleMerge['body']['error']['code']??'')==='location_cycle','cycle-producing merge did not return location_cycle');
    $projectSource=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' project source','loc_type'=>'storage_unit','assigned_project_id'=>$wp4ProjectA]);$projectTarget=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' project target','loc_type'=>'storage_unit','assigned_project_id'=>$wp4ProjectB]);$psid=(int)$projectSource['body']['data']['id'];$ptid=(int)$projectTarget['body']['data']['id'];$pp=httpRequest($base.$wp4Loc.'/'.$psid.'/merge-preview?target_id='.$ptid,'GET',null,$cookie);$pm=httpRequest($base.$wp4Loc.'/'.$psid.'/merge','POST',['target_id'=>$ptid,'fingerprint'=>$pp['body']['data']['fingerprint']],$cookie,$csrf);check($pm['status']===409&&($pm['body']['error']['code']??'')==='merge_conflict','different-project merge was accepted');

    // 037: items on catalog product types, software licences, live location codes, sub-second locks.
    $comp=$base.'/api/inventory/components';
    $isNull=static fn(array $d,string $k):bool=>array_key_exists($k,$d)&&$d[$k]===null;
    $post=static fn(array $b):array=>httpRequest($comp,'POST',$b,$cookie,$csrf);
    $mkType=static function(string $kind,string $name)use($base,$cookie,$csrf):int{$r=httpRequest($base.'/api/inventory/product-types','POST',['kind'=>$kind,'name'=>$name],$cookie,$csrf);check($r['status']===201,"037 $kind type create failed: ".json_encode($r['body']));return (int)$r['body']['data']['id'];};
    $consumable=$mkType('consumable',$i1cPrefix.' cable ties');$software=$mkType('software',$i1cPrefix.' office suite');$laptopType=$mkType('hardware',$i1cPrefix.' laptop');
    $ties=$post(['product_type_id'=>$consumable,'quantity'=>25,'location_id'=>$ids['location'],'notes'=>$i1cPrefix.' ties']);
    check($ties['status']===201&&$isNull($ties['body']['data']??[],'modelId')&&($ties['body']['data']['productTypeId']??0)===$consumable&&($ties['body']['data']['productTypeName']??'')===$i1cPrefix.' cable ties'&&($ties['body']['data']['productTypeKind']??'')==='consumable'&&($ties['body']['data']['quantity']??0)===25,'catalog-only item create failed: '.json_encode($ties['body']));
    $tiesListed=httpRequest($comp.'?product_type_id='.$consumable,'GET',null,$viewerCookie);check($tiesListed['status']===200&&array_column($tiesListed['body']['data']??[],'id')===[(int)($ties['body']['data']['id']??0)],'model-less item missing from the components list');
    $tiesSearch=httpRequest($comp.'?q='.rawurlencode($i1cPrefix.' cable').'&limit=5&offset=0','GET',null,$viewerCookie);check(($tiesSearch['body']['data']['page']['total']??null)===1,'components q did not match the product type name');
    $laptops=$post(['product_type_id'=>$laptopType,'count'=>3,'location_id'=>$ids['location'],'notes'=>$i1cPrefix.' laptops']);
    check($laptops['status']===201&&($laptops['body']['data']['count']??0)===3&&count(array_unique(array_column($laptops['body']['data']['items']??[],'id')))===3,'count=3 item create failed: '.json_encode($laptops['body']));
    $laptopId=(int)($laptops['body']['data']['items'][0]['id']??0);
    check($post(['product_type_id'=>$laptopType,'count'=>2,'serial'=>$i1cPrefix.'-S'])['status']===400,'count>1 with a serial was accepted');
    check($post(['product_type_id'=>$laptopType,'count'=>51])['status']===400,'count above the batch limit was accepted');
    $sw=$post(['product_type_id'=>$software,'licence_seats'=>5,'licence_expires_on'=>'2027-12-31','licence_reference'=>'1Password: Office family plan, order 113-4567890','installed_on_unit_id'=>$laptopId,'notes'=>$i1cPrefix.' office']);
    $swd=$sw['body']['data']??[];
    check($sw['status']===201&&($swd['licenceSeats']??0)===5&&($swd['licenceExpiresOn']??'')==='2027-12-31'&&str_starts_with((string)($swd['licenceReference']??''),'1Password')&&($swd['installedOnUnitId']??0)===$laptopId&&($swd['installedOnName']??'')===$i1cPrefix.' laptop','software item with licence create failed: '.json_encode($sw['body']));
    $swId=(int)($swd['id']??0);
    $rejects=[
        'licence on hardware'=>['product_type_id'=>$laptopType,'licence_seats'=>2],
        'install target on hardware'=>['product_type_id'=>$laptopType,'installed_on_unit_id'=>$laptopId],
        'product-key reference'=>['product_type_id'=>$software,'licence_reference'=>'ABCDE-12345-FGHIJ-67890-KLMNO'],
        'password reference'=>['product_type_id'=>$software,'licence_reference'=>'password is hunter2'],
        'token-like reference'=>['product_type_id'=>$software,'licence_reference'=>'ghp_9f8e7d6c5b4a39281706f5e4d3c2b1a0'],
        'software on software'=>['product_type_id'=>$software,'installed_on_unit_id'=>$swId],
        'install on missing unit'=>['product_type_id'=>$software,'installed_on_unit_id'=>999999999],
        'impossible expiry'=>['product_type_id'=>$software,'licence_expires_on'=>'2027-02-30'],
        'zero seats'=>['product_type_id'=>$software,'licence_seats'=>0],
        'model/type mismatch'=>['model_id'=>$ids['model'],'product_type_id'=>$consumable],
        'no identity'=>['notes'=>$i1cPrefix.' none'],
        'missing type'=>['product_type_id'=>999999999],
    ];
    foreach($rejects as $label=>$body){$r=$post($body);check($r['status']===400,"037 $label was not rejected with 400 ($r[status]): ".json_encode($r['body']));}
    $retired=$mkType('hardware',$i1cPrefix.' retired');check(httpRequest($base.'/api/inventory/product-types/'.$retired,'DELETE',[],$cookie,$csrf)['status']===200,'037 retire fixture failed');
    $onRetired=$post(['product_type_id'=>$retired]);check($onRetired['status']===400&&str_contains((string)($onRetired['body']['error']['message']??''),'retired'),'item create on a retired product type was accepted');
    $legacyItem=$post(['model_id'=>$ids['model'],'serial'=>$i1cPrefix.'-legacy','notes'=>$i1cPrefix.' legacy']);
    check($legacyItem['status']===201&&($legacyItem['body']['data']['modelId']??0)===$ids['model']&&($legacyItem['body']['data']['productTypeId']??0)===$legacyType,'legacy model_id create no longer derives the product type: '.json_encode($legacyItem['body']));
    $byType=$post(['product_type_id'=>$legacyType,'notes'=>$i1cPrefix.' by type']);check($byType['status']===201&&($byType['body']['data']['modelId']??0)===$ids['model'],'product type with a legacy model did not fill model_id');
    $dupSerial=$post(['product_type_id'=>$laptopType,'serial'=>$i1cPrefix.'-dup','notes'=>$i1cPrefix.' dup']);check($dupSerial['status']===201,'serial fixture failed');
    $dupAgain=$post(['product_type_id'=>$laptopType,'serial'=>$i1cPrefix.'-dup','notes'=>$i1cPrefix.' dup']);check($dupAgain['status']===409,'duplicate serial within a product type was not 409');
    $swPath=$comp.'/'.$swId;
    $swEdit=httpRequest($swPath,'PUT',['licence_seats'=>10,'licence_reference'=>null,'installed_on_unit_id'=>null],$cookie,$csrf);check($swEdit['status']===200&&($swEdit['body']['data']['licenceSeats']??0)===10&&$isNull($swEdit['body']['data']??[],'licenceReference')&&$isNull($swEdit['body']['data']??[],'installedOnUnitId'),'software licence edit failed: '.json_encode($swEdit['body']));
    check(httpRequest($swPath,'PUT',['product_type_id'=>$laptopType],$cookie,$csrf)['status']===400,'changing a licensed item to hardware kept its licence fields');
    check(httpRequest($comp.'/'.$laptopId,'PUT',['licence_seats'=>3],$cookie,$csrf)['status']===400,'licence edit on a hardware item was accepted');
    check(httpRequest($comp.'/'.$laptopId,'PUT',['product_type_id'=>null],$cookie,$csrf)['status']===400,'product_type_id was cleared');
    check(httpRequest($comp.'/'.$laptopId,'PUT',['product_type_id'=>$retired],$cookie,$csrf)['status']===400,'item moved onto a retired product type');
    $retype=httpRequest($comp.'/'.(int)($ties['body']['data']['id']??0),'PUT',['product_type_id'=>$legacyType],$cookie,$csrf);check($retype['status']===200&&($retype['body']['data']['modelId']??0)===$ids['model'],'re-typing an item did not follow the catalog');
    $unModel=httpRequest($retype['status']===200?$comp.'/'.(int)$retype['body']['data']['id']:$comp.'/0','PUT',['model_id'=>null],$cookie,$csrf);check($unModel['status']===200&&$isNull($unModel['body']['data']??[],'modelId')&&($unModel['body']['data']['productTypeId']??0)===$legacyType,'clearing the legacy model failed or lost the product type');

    // Live location codes are globally unique; empty codes repeat (037).
    $codeA='C'.$suffix;
    $withCode=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' coded','loc_type'=>'storage_unit','code'=>$codeA]);check($withCode['status']===201,'coded location create failed');
    foreach([$codeA,strtolower($codeA)] as $dupCode){$r=$createLoc(['parent_id'=>$wp4Floor,'name'=>$wp4Prefix.' coded '.$dupCode,'loc_type'=>'room','code'=>$dupCode]);check($r['status']===409&&($r['body']['error']['code']??'')==='duplicate_location'&&str_contains((string)($r['body']['error']['message']??''),'code'),"duplicate live code $dupCode was not refused: ".json_encode($r['body']));}
    foreach([1,2] as $n){$r=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' blank code '.$n,'loc_type'=>'storage_unit','code'=>'']);check($r['status']===201&&$isNull($r['body']['data']??[],'code'),'empty location code was not stored as NULL / did not repeat');}
    $plainLoc=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' to recode','loc_type'=>'storage_unit']);$plainLocId=(int)$plainLoc['body']['data']['id'];
    $recode=httpRequest($base.$wp4Loc.'/'.$plainLocId,'PUT',['code'=>$codeA,'updated_at'=>(string)$plainLoc['body']['data']['updatedAt']],$cookie,$csrf);check($recode['status']===409&&($recode['body']['error']['code']??'')==='duplicate_location'&&str_contains((string)($recode['body']['error']['message']??''),'code'),'location edit to a used code was not refused: '.json_encode($recode['body']));
    $bulkSeed=$createLoc(['parent_id'=>$wp4Root,'name'=>$wp4Prefix.' bulk seed','loc_type'=>'storage_unit','code'=>'B'.$suffix.'7']);check($bulkSeed['status']===201,'bulk code seed failed');
    $bulkDup=httpRequest($base.$wp4Loc.'/bulk-storage-units','POST',['parent_id'=>$wp4Root,'count'=>2,'prefix'=>$wp4Prefix.' bulk code ','start'=>1,'code_prefix'=>'B'.$suffix,'code_start'=>6],$cookie,$csrf);check($bulkDup['status']===409&&($bulkDup['body']['error']['code']??'')==='duplicate_location'&&str_contains((string)($bulkDup['body']['error']['message']??''),'already used'),'bulk create reusing a live code was not refused: '.json_encode($bulkDup['body']));
    check((int)$db->query('SELECT COUNT(*) FROM locations WHERE name LIKE '.$db->quote($wp4Prefix.' bulk code %'))->fetchColumn()===0,'refused bulk create left partial rows');

    // Two edits inside one second: the first token is stale after the second (037 DATETIME(6)).
    while(fmod(microtime(true),1.0)>0.5)usleep(20000);
    $tokA=(string)$plainLoc['body']['data']['updatedAt'];check(preg_match('/\.\d{6}Z\z/',$tokA)===1,'location updatedAt token lacks microseconds: '.$tokA);
    $e1=httpRequest($base.$wp4Loc.'/'.$plainLocId,'PUT',['notes'=>'first','updated_at'=>$tokA],$cookie,$csrf);check($e1['status']===200,'first same-second location edit failed');$tokB=(string)($e1['body']['data']['updatedAt']??'');
    $e2=httpRequest($base.$wp4Loc.'/'.$plainLocId,'PUT',['notes'=>'second','updated_at'=>$tokB],$cookie,$csrf);check($e2['status']===200,'second same-second location edit failed');$tokC=(string)($e2['body']['data']['updatedAt']??'');
    check(substr($tokB,0,19)===substr($tokC,0,19)&&$tokB!==$tokC,"location edits were not in one second with distinct tokens ($tokB, $tokC)");
    check(httpRequest($base.$wp4Loc.'/'.$plainLocId,'PUT',['notes'=>'stale','updated_at'=>$tokB],$cookie,$csrf)['status']===409,'stale same-second location token was accepted');
    while(fmod(microtime(true),1.0)>0.5)usleep(20000);
    $ptPath='/api/inventory/product-types/'.$consumable;
    $p1=httpRequest($base.$ptPath,'PUT',['notes'=>'first','updated_at'=>$typeToken($ptPath)],$cookie,$csrf);check($p1['status']===200,'first same-second type edit failed');$ptB=(string)($p1['body']['data']['updated_at']??'');
    $p2=httpRequest($base.$ptPath,'PUT',['notes'=>'second','updated_at'=>$ptB],$cookie,$csrf);check($p2['status']===200,'second same-second type edit failed: '.json_encode($p2['body']));$ptC=(string)($p2['body']['data']['updated_at']??'');
    check(substr($ptB,0,19)===substr($ptC,0,19)&&$ptB!==$ptC&&strlen($ptB)===26,"type edits were not in one second with distinct DATETIME(6) tokens ($ptB, $ptC)");
    check(httpRequest($base.$ptPath,'PUT',['notes'=>'stale','updated_at'=>$ptB],$cookie,$csrf)['status']===409,'stale same-second product type token was accepted');

    $overflow=$db->prepare("INSERT INTO receipts(vendor,currency) VALUES(?,'USD')");for($i=0;$i<501;$i++)$overflow->execute([$vendor]);$tooBig=httpRequest($base.'/api/inventory','GET',null,$cookie);check($tooBig['status']===413&&($tooBig['body']['error']['code']??null)==='inventory_page_required','501 receipts did not require paginated inventory');
    echo "inventory HTTP baseline and WP4 location assertions passed (".($GLOBALS['inventory_http_assertions']??0)." checks)\n";
} catch(Throwable $e){fwrite(STDERR,'Inventory HTTP test failed: '.$e->getMessage()."\n");$failure=$e;} finally {if(is_resource($server)){proc_terminate($server);proc_close($server);} $cleanup();if(is_dir($work)){$it=new RecursiveIteratorIterator(new RecursiveDirectoryIterator($work,FilesystemIterator::SKIP_DOTS),RecursiveIteratorIterator::CHILD_FIRST);foreach($it as $p)$p->isDir()?rmdir($p->getPathname()):unlink($p->getPathname());rmdir($work);}}
if($failure!==null)exit(1);
