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
$ids = ['location'=>null,'model'=>null,'unit'=>null,'barcode'=>null,'receipt'=>null,'entity'=>null,'conflict'=>null];
$work = sys_get_temp_dir().'/solarinet-inventory-http-'.bin2hex(random_bytes(8)); $sessions=$work.'/sessions'; $server=null; $failure=null;
$cleanup = static function () use ($db,&$ids,$tag,$vendor,$make,$model,$loc): void {
    foreach (['barcode_scans'=>"code=".$db->quote($tag),'receipts'=>"vendor=".$db->quote($vendor)] as $table=>$where) { try {$db->exec("DELETE FROM $table WHERE $where");} catch (Throwable $e) {} }
    foreach ([['entity_hardware_unit_links','hardware_unit_id',$ids['unit']], ['entities','id',$ids['entity']], ['entities','id',$ids['conflict']], ['barcodes','id',$ids['barcode']], ['hardware_units','id',$ids['unit']], ['hardware_models','id',$ids['model']], ['locations','id',$ids['location']]] as [$table,$column,$id]) { if ($id) { try {$q=$db->prepare("DELETE FROM $table WHERE $column=?");$q->execute([$id]);}catch(Throwable $e){} } }
    try { if ($ids['model']) $db->prepare('DELETE FROM rw_model_groups WHERE hardware_model_id=?')->execute([$ids['model']]); if ($ids['unit']) $db->prepare("DELETE FROM audit_log WHERE (table_name='hardware_units' AND row_id=?) OR (table_name='hardware_models' AND row_id=?) OR (table_name='entities' AND row_id IN (?,?))")->execute([$ids['unit'],$ids['model'],$ids['entity'] ?: 0,$ids['conflict'] ?: 0]); $db->prepare('DELETE FROM receipts WHERE vendor=?')->execute([$vendor]);$db->prepare('DELETE FROM hardware_units WHERE asset_tag=?')->execute([$tag]);$db->prepare('DELETE FROM hardware_models WHERE make=? AND model=?')->execute([$make,$model]);$db->prepare('DELETE FROM locations WHERE name=?')->execute([$loc]);}catch(Throwable $e){}
};
try {
    $marker=$db->query("SELECT marker FROM _solarinet_test_scratch WHERE marker='solarinet-ci-scratch-v1'")->fetchColumn(); check($marker==='solarinet-ci-scratch-v1','disposable scratch SoR marker is missing');
    check(getenv('SOLARI_TEST_SOR_DB') === '1' && in_array($host,['127.0.0.1','localhost'],true) && preg_match('/^solarinet_[a-z0-9_]+$/',$name) && ctype_digit($port), 'scratch SoR guard failed');
    $manual=(int)$db->query("SELECT id FROM sources WHERE slug='manual'")->fetchColumn(); check($manual>0,'manual source missing');
    $db->prepare("INSERT INTO locations(name,loc_type,source_id,asserted_kind,asserted_at) VALUES(?,'site',?,'human',NOW())")->execute([$loc,$manual]); $ids['location']=(int)$db->lastInsertId();
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
    foreach(['/api/inventory/models','/api/inventory/components','/api/inventory/receipts','/api/inventory/projects','/api/inventory/locations','/api/inventory/allocations'] as $p){$r=httpRequest($base.$p.'?limit=1&offset=0','GET',null,$cookie);check($r['status']===200&&is_array($r['body']['data']['items']??null)&&isset($r['body']['data']['page']['total']),"paged endpoint failed: $p");}
    check(httpRequest($base.'/api/inventory/models?limit=0','GET',null,$cookie)['status']===400,'limit=0 was accepted');
    $aggregate=httpRequest($base.'/api/inventory','GET',null,$cookie);$d=$aggregate['body']['data']??[];check($aggregate['status']===200&&is_array($d['models']??null)&&is_array($d['units']??null)&&!isset($d['page']),'combined inventory shape changed');
    $overflow=$db->prepare("INSERT INTO receipts(vendor,currency) VALUES(?,'USD')");for($i=0;$i<501;$i++)$overflow->execute([$vendor]);$tooBig=httpRequest($base.'/api/inventory','GET',null,$cookie);check($tooBig['status']===413&&($tooBig['body']['error']['code']??null)==='inventory_page_required','501 receipts did not require paginated inventory');
    echo "inventory HTTP entity linking, paging, aggregate bound, RackWire profile, barcode lookup, and retired-route assertions passed\n";
} catch(Throwable $e){fwrite(STDERR,'Inventory HTTP test failed: '.$e->getMessage()."\n");$failure=$e;} finally {if(is_resource($server)){proc_terminate($server);proc_close($server);} $cleanup();if(is_dir($work)){$it=new RecursiveIteratorIterator(new RecursiveDirectoryIterator($work,FilesystemIterator::SKIP_DOTS),RecursiveIteratorIterator::CHILD_FIRST);foreach($it as $p)$p->isDir()?rmdir($p->getPathname()):unlink($p->getPathname());rmdir($work);}}
if($failure!==null)exit(1);
