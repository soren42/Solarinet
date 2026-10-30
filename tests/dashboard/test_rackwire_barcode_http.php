<?php
declare(strict_types=1);
/* Confirm the retired scanner import endpoint returns 404 to an operator. */
$work = sys_get_temp_dir() . '/solarinet-retired-route-' . bin2hex(random_bytes(6));
$sessions = $work . '/sessions';
mkdir($work, 0700); mkdir($sessions, 0700);
$api = var_export(__DIR__ . '/../../dashboard/api/index.php', true);
$auth = var_export(__DIR__ . '/../../dashboard/api/lib/Auth.php', true);
$router = "<?php\n\$path=parse_url(\$_SERVER['REQUEST_URI']??'/',PHP_URL_PATH);\n"
    . "if(\$path==='/__session'){require_once $auth;Auth::establishSession(['username'=>'retired-route-test','role'=>'operator','source'=>'directory']);header('Content-Type: application/json');echo json_encode(['csrf'=>Auth::csrfToken()]);exit;}\n"
    . "if(is_string(\$path)&&str_starts_with(\$path,'/api/')){require $api;exit;} http_response_code(404);";
file_put_contents($work . '/router.php', $router);
$sock = stream_socket_server('tcp://127.0.0.1:0', $errno, $error);
if (!$sock) throw new RuntimeException('Could not reserve loopback port');
$addr = stream_socket_get_name($sock, false); fclose($sock); $port = (int)substr(strrchr($addr, ':'), 1);
$server = proc_open([PHP_BINARY, '-d', 'session.save_path='.$sessions, '-S', "127.0.0.1:$port", $work.'/router.php'], [0=>['file','/dev/null','r'],1=>['file','/dev/null','a'],2=>['file','/dev/null','a']], $pipes);
try {
    usleep(200000); $base="http://127.0.0.1:$port";
    file_get_contents($base.'/__session'); $cookie='';
    foreach ($http_response_header ?? [] as $h) if (stripos($h,'Set-Cookie:')===0) $cookie=trim(explode(';',substr($h,11))[0]);
    $csrf=json_decode((string)file_get_contents($base.'/__session'),true)['csrf']??'';
    $ctx=stream_context_create(['http'=>['method'=>'POST','ignore_errors'=>true,'header'=>"Cookie: $cookie\r\nContent-Type: application/json\r\nOrigin: $base\r\nX-Solari-CSRF: $csrf\r\n",'content'=>'{}']]);
    file_get_contents($base.'/api/inventory/scanner-import/preview',false,$ctx);
    if (!isset($http_response_header[0]) || !preg_match('/ 404 /',$http_response_header[0])) throw new RuntimeException('Operator scanner-import request did not return 404');
    echo "operator scanner-import route returns 404\n";
} finally { proc_terminate($server); proc_close($server); foreach(glob($sessions.'/*')?:[] as $f) unlink($f); @rmdir($sessions); @unlink($work.'/router.php'); @rmdir($work); }
