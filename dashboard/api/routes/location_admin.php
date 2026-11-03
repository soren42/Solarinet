<?php
declare(strict_types=1);

/** WP4 location move, merge, assignment, and container-type API. */
return static function (Router $router): void {
    $read = Policy::INVENTORY_CATALOG_READ;
    $write = Policy::INVENTORY_CATALOG_WRITE;

    $router->post('/api/inventory/locations/{id}/move', static function (array $p): void {
        $id = Inv::idParam($p['id']); $body = solari_json_body();
        if (!array_key_exists('parent_id', $body)) Response::error('bad_request', 'parent_id is required (nullable).', 400);
        $parentId = loc_optional_id($body['parent_id'], 'parent_id');
        $source = Sor::dashboardSourceId();
        $result = loc_transaction(static function (PDO $db) use ($id, $parentId, $source): array {
            $loc = loc_lock($db, $id);
            if ($loc === null) loc_error('not_found', "No location $id", 404);
            $oldParent = $loc['parent_id'] === null ? null : (int)$loc['parent_id'];
            loc_walk_ancestors($db, $parentId, $id);
            $newParent = $parentId === null ? null : loc_lock($db, $parentId);
            if ($parentId !== null && ($newParent === null || $newParent['deleted_at'] !== null)) loc_error('not_found', "No parent location $parentId", 404);
            loc_transition_rows($db, $parentId, (string)$loc['loc_type']);
            loc_sibling_available($db, (string)$loc['name'], $parentId, $id);
            $st = $db->prepare("UPDATE locations SET parent_id=:parent,asserted_kind='human',asserted_at=NOW() WHERE id=:id");
            $st->execute([':parent'=>$parentId, ':id'=>$id]);
            loc_audit($db, 'locations', $id, 'update', ['parent_id'=>$oldParent], ['parent_id'=>$parentId], 'Location move', $source);
            return ['id'=>$id, 'parent_id'=>$parentId, 'old_parent_id'=>$oldParent];
        });
        Response::ok($result);
    }, $write);

    $router->get('/api/inventory/locations/{id}/merge-preview', static function (array $p): void {
        $id = Inv::idParam($p['id']); $target = loc_query_id($_GET['target_id'] ?? null, 'target_id');
        if ($id === $target) Response::error('bad_request', 'Source and target must differ.', 400);
        $db = Sor::db();
        $sourceRow = loc_lockless_row($db, $id); $targetRow = loc_lockless_row($db, $target);
        if ($sourceRow === null || $targetRow === null) Response::error('not_found', 'Source or target location not found.', 404);
        $counts = loc_merge_counts($db, $id);
        $fingerprint = loc_merge_fingerprint($db, $sourceRow, $targetRow, $counts);
        Response::ok(['source'=>loc_public_location($sourceRow), 'target'=>loc_public_location($targetRow), 'counts'=>$counts, 'fingerprint'=>$fingerprint]);
    }, $read);

    $router->post('/api/inventory/locations/{id}/merge', static function (array $p): void {
        $id = Inv::idParam($p['id']); $body = solari_json_body();
        $target = loc_required_id($body, 'target_id'); $fingerprint = $body['fingerprint'] ?? null;
        if (!is_string($fingerprint) || !preg_match('/\A[a-f0-9]{64}\z/', $fingerprint)) Response::error('bad_request', 'fingerprint is required.', 400);
        if ($id === $target) Response::error('bad_request', 'Source and target must differ.', 400);
        $sourceId = Sor::dashboardSourceId();
        $result = loc_transaction(static function (PDO $db) use ($id,$target,$fingerprint,$sourceId): array {
            $source = loc_lock($db,$id); $dest = loc_lock($db,$target);
            if ($source === null || $source['deleted_at'] !== null) {
                $done = loc_completed_merge($db,$id,$target);
                if ($done) return ['source_id'=>$id,'target_id'=>$target,'merged'=>true,'idempotent'=>true,'counts'=>[]];
                loc_error('not_found','Source location not found.',404);
            }
            if ($dest === null || $dest['deleted_at'] !== null) loc_error('not_found','Target location not found.',404);
            $fkRows = loc_discover_fks($db);
            foreach ($fkRows as $fk) if (!loc_fk_supported($fk)) loc_error('merge_conflict','Merge refused: unsupported location reference '.$fk['table_name'].'.'.$fk['column_name'],409);
            loc_lock_merge_refs($db,$id,$fkRows);
            $counts = loc_merge_counts($db,$id);
            $actual = loc_merge_fingerprint($db,$source,$dest,$counts);
            if (!hash_equals($fingerprint,$actual)) loc_error('merge_conflict','Location references changed after preview.',409);
            if ($source['loc_type'] !== $dest['loc_type']) loc_error('merge_conflict','Locations must have the same level.',409);
            if ($source['assigned_project_id'] !== null && $dest['assigned_project_id'] !== null && (int)$source['assigned_project_id'] !== (int)$dest['assigned_project_id']) loc_error('merge_conflict','Locations are assigned to different projects.',409);
            $children = $db->prepare('SELECT id,name,loc_type,parent_id FROM locations WHERE parent_id=:id AND deleted_at IS NULL ORDER BY id FOR UPDATE');
            $children->execute([':id'=>$id]); $childRows=$children->fetchAll();
            $seen=[];
            foreach ($childRows as $child) {
                $key=mb_strtolower((string)$child['name']);
                if (isset($seen[$key])) loc_error('merge_conflict','Source has duplicate live child names.',409);
                $seen[$key]=true;
                if(loc_sibling_collision($db,(string)$child['name'],$target,(int)$child['id']))loc_error('merge_conflict','A source child name already exists under the target.',409);
                loc_transition_rows($db,$target,(string)$child['loc_type']);
                loc_walk_ancestors($db,$target,(int)$child['id']);
            }
            // A target that is itself in the source subtree would form a cycle.
            loc_walk_ancestors($db,$target,$id);
            $moved=[];
            foreach ($fkRows as $fk) {
                $table=(string)$fk['table_name']; $column=(string)$fk['column_name'];
                if ($table==='locations' && $column==='parent_id') {
                    $st=$db->prepare("UPDATE locations SET parent_id=:target,asserted_kind='human',asserted_at=NOW() WHERE parent_id=:source AND deleted_at IS NULL");
                    $st->execute([':target'=>$target,':source'=>$id]);
                    $moved[$table.'.'.$column]=$st->rowCount();
                    foreach($childRows as $child) loc_audit($db,'locations',(int)$child['id'],'update',['parent_id'=>$id],['parent_id'=>$target],'Child moved by location merge',$sourceId);
                    continue;
                }
                if (!loc_fk_supported($fk)) loc_error('merge_conflict','Merge refused: unsupported location reference.',409);
                $q='SELECT COUNT(*) FROM `'.str_replace('`','``',$table).'` WHERE `'.str_replace('`','``',$column).'`=:id';
                $st=$db->prepare($q); $st->execute([':id'=>$id]); $n=(int)$st->fetchColumn();
                if ($n===0) continue;
                $st=$db->prepare('UPDATE `'.str_replace('`','``',$table).'` SET `'.str_replace('`','``',$column).'`=:target WHERE `'.str_replace('`','``',$column).'`=:source');
                $st->execute([':target'=>$target,':source'=>$id]);
                $moved[$table.'.'.$column]=$n;
            }
            // Polymorphic history is counted for preview but deliberately remains an immutable snapshot.
            $liveBarcode=$db->prepare("SELECT id,code FROM barcodes WHERE subject_table='locations' AND subject_id=:id AND deleted_at IS NULL FOR UPDATE");
            $liveBarcode->execute([':id'=>$id]); $bars=$liveBarcode->fetchAll();
            foreach ($bars as $bar) {
                $dupe=$db->prepare('SELECT id FROM barcodes WHERE code=:code AND deleted_at IS NULL AND id<>:id LIMIT 1 FOR UPDATE');
                $dupe->execute([':code'=>$bar['code'],':id'=>$bar['id']]);
                if ($dupe->fetchColumn() !== false) loc_error('merge_conflict','A live barcode code cannot be safely retargeted.',409);
            }
            if ($bars!==[]) $db->prepare("UPDATE barcodes SET subject_id=:target,asserted_kind='human',asserted_at=NOW() WHERE subject_table='locations' AND subject_id=:source AND deleted_at IS NULL")->execute([':target'=>$target,':source'=>$id]);
            $srcProject=$source['assigned_project_id']; $dstProject=$dest['assigned_project_id'];
            $effectiveProject=$dstProject===null?$srcProject:$dstProject;
            if ($effectiveProject!==$dstProject) $db->prepare("UPDATE locations SET assigned_project_id=:project,asserted_kind='human',asserted_at=NOW() WHERE id=:id")->execute([':project'=>$effectiveProject,':id'=>$target]);
            $db->prepare("UPDATE locations SET deleted_at=NOW(),asserted_kind='human',asserted_at=NOW() WHERE id=:id")->execute([':id'=>$id]);
            loc_audit($db,'locations',$target,'update',['assigned_project_id'=>$dstProject],['assigned_project_id'=>$effectiveProject],'Location merge target', $sourceId);
            loc_audit($db,'locations',$id,'soft_delete',['deleted_at'=>null,'parent_id'=>$source['parent_id']],['deleted_at'=>'merged','merge_target_id'=>$target],'Merged into location '.$target,$sourceId);
            foreach ($moved as $key=>$count) loc_audit($db,'locations',$id,'update',null,['merge_references'=>[$key=>$count],'merge_target_id'=>$target],'Location merge references', $sourceId);
            return ['source_id'=>$id,'target_id'=>$target,'merged'=>true,'idempotent'=>false,'counts'=>$counts,'moved'=>$moved];
        });
        Response::ok($result);
    }, $write);

    $router->post('/api/inventory/locations/bulk-storage-units', static function (): void {
        $body=solari_json_body(); $parent=loc_required_nullable_id($body,'parent_id');
        $count=$body['count']??null;
        if (!is_int($count) || $count<1 || $count>500) Response::error('bad_request','count must be an integer from 1 to 500.',400);
        $names=$body['names']??null;
        if ($names!==null) {
            if (!is_array($names)||!array_is_list($names)||count($names)!==$count) Response::error('bad_request','names must contain exactly count strings.',400);
            $names=array_map(static fn($v)=>loc_name_value($v),$names);
        } else {
            $prefix=$body['prefix']??null; $start=$body['start']??null;
            if (!is_string($prefix)||trim($prefix)===''||strlen(trim($prefix))>100||!is_int($start)||$start<0) Response::error('bad_request','Supply explicit names or prefix and non-negative integer start.',400);
            $prefix=trim($prefix); $names=[];
            for($i=0;$i<$count;$i++) $names[]=$prefix.($start+$i);
        }
        $codePrefix=$body['code_prefix']??null; $codeStart=$body['code_start']??0;
        if ($codePrefix!==null && (!is_string($codePrefix)||trim($codePrefix)===''||strlen(trim($codePrefix))>20||!is_int($codeStart)||$codeStart<0)) Response::error('bad_request','code_prefix/code_start is invalid.',400);
        $codes=[];
        foreach($names as $i=>$name) { $code=$codePrefix===null?null:trim($codePrefix).($codeStart+$i); if($code!==null&&strlen($code)>32) Response::error('bad_request','Generated code exceeds 32 characters.',400); $codes[]=$code; }
        $ct=loc_optional_id($body['container_type_id']??null,'container_type_id'); $project=loc_optional_id($body['project_id']??null,'project_id');
        $source=Sor::dashboardSourceId();
        $result=loc_transaction(static function(PDO $db)use($parent,$names,$codes,$ct,$project,$source):array{
            $parentRow=$parent===null?null:loc_lock($db,$parent);
            if($parent!==null&&($parentRow===null||$parentRow['deleted_at']!==null))loc_error('not_found','Parent location not found.',404);
            loc_transition_rows($db,$parent,'storage_unit');
            if($ct!==null&&!loc_live_row($db,'container_types',$ct))loc_error('not_found','Container type not found.',404);
            if($project!==null&&!loc_live_row($db,'projects',$project))loc_error('not_found','Project not found.',404);
            $seen=[]; foreach($names as $name){$key=mb_strtolower($name);if(isset($seen[$key]))loc_error('duplicate_location','Names collide within this batch.',409);$seen[$key]=true;loc_sibling_available($db,$name,$parent,null);}
            $seenCodes=[]; foreach($codes as $code)if($code!==null){$key=mb_strtolower($code);if(isset($seenCodes[$key]))loc_error('duplicate_location','Codes collide within this batch.',409);$seenCodes[$key]=true;$st=$db->prepare('SELECT id FROM locations WHERE code=:code LIMIT 1 FOR UPDATE');$st->execute([':code'=>$code]);if($st->fetchColumn()!==false)loc_error('duplicate_location','A location code already exists.',409);}
            $st=$db->prepare("INSERT INTO locations(parent_id,name,loc_type,code,container_type_id,assigned_project_id,source_id,asserted_kind,asserted_at) VALUES(:parent,:name,'storage_unit',:code,:ct,:project,:source,'human',NOW())");$ids=[];
            foreach($names as $i=>$name){$st->execute([':parent'=>$parent,':name'=>$name,':code'=>$codes[$i],':ct'=>$ct,':project'=>$project,':source'=>$source]);$id=(int)$db->lastInsertId();$ids[]=$id;loc_audit($db,'locations',$id,'insert',null,['parent_id'=>$parent,'name'=>$name,'loc_type'=>'storage_unit','code'=>$codes[$i],'container_type_id'=>$ct,'assigned_project_id'=>$project],'Bulk-created storage unit',$source);}
            return ['ids'=>$ids,'count'=>count($ids)];
        });
        Response::ok($result,201);
    },$write);

    $router->add('PUT','/api/inventory/locations/{id}/project',static function(array $p):void{
        $id=Inv::idParam($p['id']);$body=solari_json_body();$project=loc_required_nullable_id($body,'project_id');$source=Sor::dashboardSourceId();
        $result=loc_transaction(static function(PDO $db)use($id,$project,$source):array{
            $row=loc_lock($db,$id);if($row===null||$row['deleted_at']!==null)loc_error('not_found','Location not found.',404);
            if($row['loc_type']!=='storage_unit')loc_error('bad_request','Project assignment requires a storage_unit.',400);
            if($project!==null&&!loc_live_row($db,'projects',$project))loc_error('not_found','Project not found.',404);
            $old=$row['assigned_project_id']===null?null:(int)$row['assigned_project_id'];
            if($project===null)loc_assert_container_empty($db,$id);
            $db->prepare("UPDATE locations SET assigned_project_id=:project,asserted_kind='human',asserted_at=NOW() WHERE id=:id")->execute([':project'=>$project,':id'=>$id]);
            loc_audit($db,'locations',$id,'update',['assigned_project_id'=>$old],['assigned_project_id'=>$project],'Container project assignment',$source);
            return ['id'=>$id,'project_id'=>$project];
        });Response::ok($result);
    },$write);

    $router->get('/api/inventory/container-types',static function():void{
        if(isset($_GET['include_deleted'])&&!in_array($_GET['include_deleted'],['0','1'],true))Response::error('bad_request','include_deleted must be 1 or 0.',400);
        $include=($_GET['include_deleted']??'')==='1';$sql='SELECT id,name,created_at,updated_at,deleted_at FROM container_types'.($include?'':' WHERE deleted_at IS NULL').' ORDER BY name,id';Response::ok(array_map('loc_type_row',Inv::rows($sql)));
    },$read);
    $router->post('/api/inventory/container-types',static function():void{
        $body=solari_json_body();$name=loc_name_value($body['name']??null);$source=Sor::dashboardSourceId();
        try{$id=(int)Inv::transaction(static function(PDO $db)use($name,$source):int{$st=$db->prepare("INSERT INTO container_types(name,source_id,asserted_kind,asserted_at) VALUES(:name,:source,'human',NOW())");$st->execute([':name'=>$name,':source'=>$source]);$id=(int)$db->lastInsertId();loc_audit($db,'container_types',$id,'insert',null,['name'=>$name],'Container type create',$source);return $id;});}catch(PDOException $e){loc_map_exception($e);throw $e;}Response::ok(loc_type_row(Inv::row('SELECT id,name,created_at,updated_at,deleted_at FROM container_types WHERE id=:id',[':id'=>$id])),201);
    },$write);
    $router->add('PUT','/api/inventory/container-types/{id}',static function(array $p):void{
        $id=Inv::idParam($p['id']);$body=solari_json_body();if(!is_string($body['updated_at']??null))Response::error('bad_request','updated_at is required.',400);$name=loc_name_value($body['name']??null);$source=Sor::dashboardSourceId();
        loc_transaction(static function(PDO $db)use($id,$body,$name,$source):void{$st=$db->prepare('SELECT id,name,updated_at FROM container_types WHERE id=:id AND deleted_at IS NULL FOR UPDATE');$st->execute([':id'=>$id]);$old=$st->fetch();if(!$old)loc_error('not_found','Container type not found.',404);if($old['updated_at']!==$body['updated_at'])loc_error('conflict','Container type changed since it was read.',409);$db->prepare("UPDATE container_types SET name=:name,asserted_kind='human',asserted_at=NOW() WHERE id=:id AND updated_at=:updated")->execute([':name'=>$name,':id'=>$id,':updated'=>$body['updated_at']]);loc_audit($db,'container_types',$id,'update',['name'=>$old['name']],['name'=>$name],'Container type rename',$source);});
        Response::ok(loc_type_row(Inv::row('SELECT id,name,created_at,updated_at,deleted_at FROM container_types WHERE id=:id',[':id'=>$id])));
    },$write);
    $router->add('DELETE','/api/inventory/container-types/{id}',static function(array $p):void{
        $id=Inv::idParam($p['id']);$source=Sor::dashboardSourceId();loc_transaction(static function(PDO $db)use($id,$source):void{$st=$db->prepare('SELECT id,name FROM container_types WHERE id=:id AND deleted_at IS NULL FOR UPDATE');$st->execute([':id'=>$id]);$row=$st->fetch();if(!$row)loc_error('not_found','Container type not found.',404);$db->prepare("UPDATE container_types SET deleted_at=NOW(),asserted_kind='human',asserted_at=NOW() WHERE id=:id")->execute([':id'=>$id]);loc_audit($db,'container_types',$id,'soft_delete',['deleted_at'=>null],['deleted_at'=>'retired','name'=>$row['name']],'Container type retire',$source);});Response::ok(['id'=>$id,'status'=>'deleted']);
    },$write);
};

function loc_error(string $code,string $message,int $status): never { Response::error($code,$message,$status); }
function loc_transaction(callable $fn) { try{return Inv::transaction($fn);}catch(PDOException $e){loc_map_exception($e);throw $e;} }
function loc_map_exception(PDOException $e): never { $state=(string)$e->getCode();$message=(string)($e->errorInfo[2]??$e->getMessage());$errno=(int)($e->errorInfo[1]??0);if($errno===1062)loc_error('duplicate_location','A location name or code already exists.',409);if($state==='23000')loc_error('conflict','A related database constraint prevented the change.',409);if($state==='45000'){if(stripos($message,'cycle')!==false||stripos($message,'depth')!==false)loc_error('location_cycle','Location hierarchy cycle rejected.',409);if(stripos($message,'duplicate')!==false)loc_error('duplicate_location','A live sibling location already exists.',409);loc_error('bad_request','Location level transition is invalid.',400);}throw $e; }
function loc_optional_id($v,string $field):?int{if($v===null||$v==='')return null;if(!is_int($v)&&!(is_string($v)&&ctype_digit($v)))loc_error('bad_request',"$field must be a positive integer or null.",400);$n=(int)$v;if($n<1)loc_error('bad_request',"$field must be a positive integer or null.",400);return$n;}
function loc_required_id(array $b,string $f):int{if(!array_key_exists($f,$b))loc_error('bad_request',"$f is required.",400);$v=loc_optional_id($b[$f],$f);if($v===null)loc_error('bad_request',"$f is required.",400);return$v;}
function loc_required_nullable_id(array $b,string $f):?int{if(!array_key_exists($f,$b))loc_error('bad_request',"$f is required and may be null.",400);return loc_optional_id($b[$f],$f);}
function loc_query_id($v,string $f):int{if(!is_string($v)||!ctype_digit($v)||(int)$v<1)loc_error('bad_request',"$f must be a positive integer.",400);return(int)$v;}
function loc_name_value($v):string{if(!is_string($v))loc_error('bad_request','name must be a string.',400);$v=trim($v);if($v===''||mb_strlen($v)>128)loc_error('bad_request','name must contain 1 to 128 characters.',400);return$v;}
function loc_live_row(PDO $db,string $table,int $id):bool{if(!in_array($table,['projects','container_types'],true))return false;$st=$db->prepare("SELECT id FROM `$table` WHERE id=:id AND deleted_at IS NULL");$st->execute([':id'=>$id]);return$st->fetchColumn()!==false;}
function loc_lock(PDO $db,int $id):?array{$st=$db->prepare('SELECT * FROM locations WHERE id=:id FOR UPDATE');$st->execute([':id'=>$id]);$r=$st->fetch();return$r?:null;}
function loc_lockless_row(PDO $db,int $id):?array{$st=$db->prepare('SELECT * FROM locations WHERE id=:id AND deleted_at IS NULL');$st->execute([':id'=>$id]);$r=$st->fetch();return$r?:null;}
function loc_walk_ancestors(PDO $db,?int $parent,int $source):void{$seen=[];$steps=0;while($parent!==null){if($parent===$source||isset($seen[$parent]))loc_error('location_cycle','Move would create or traverse a parent cycle.',409);if(++$steps>10000)loc_error('location_cycle','Location ancestor chain exceeds the depth limit.',409);$seen[$parent]=true;$row=loc_lock($db,$parent);if($row===null||$row['deleted_at']!==null)loc_error('not_found','Parent location not found.',404);$parent=$row['parent_id']===null?null:(int)$row['parent_id'];}}
function loc_transition_rows(PDO $db,?int $parent,string $type):void{if($parent===null){if($type!=='property')loc_error('bad_request','A root location must be a property.',400);return;}$st=$db->prepare('SELECT loc_type,deleted_at FROM locations WHERE id=:id FOR UPDATE');$st->execute([':id'=>$parent]);$r=$st->fetch();if(!$r||$r['deleted_at']!==null)loc_error('not_found','Parent location not found.',404);if($type==='storage_unit')return;$ok=['property'=>'floor','floor'=>'room','room'=>'section'];if(($ok[$r['loc_type']]??null)!==$type)loc_error('bad_request','Location level transition is invalid.',400);}
function inv_location_validate_children(int $id,string $type):void{$db=Sor::db();$st=$db->prepare('SELECT loc_type FROM locations WHERE parent_id=:id AND deleted_at IS NULL FOR UPDATE');$st->execute([':id'=>$id]);foreach($st->fetchAll()as$r){$child=(string)$r['loc_type'];if($child==='storage_unit')continue;$ok=['property'=>'floor','floor'=>'room','room'=>'section'];if(($ok[$type]??null)!==$child)loc_error('bad_request','Location level change would invalidate a child.',400);}}
function loc_assert_container_empty(PDO $db,int $id):void{$children=$db->prepare('SELECT id FROM locations WHERE parent_id=:id AND deleted_at IS NULL LIMIT 1 FOR UPDATE');$children->execute([':id'=>$id]);$hasChild=$children->fetchColumn()!==false;$units=$db->prepare('SELECT id FROM hardware_units WHERE location_id=:id AND deleted_at IS NULL LIMIT 1 FOR UPDATE');$units->execute([':id'=>$id]);if($hasChild||$units->fetchColumn()!==false)loc_error('container_not_empty','Container has live children or hardware units.',409);}
function loc_sibling_collision(PDO $db,string $name,?int $parent,?int $except):bool{$sql='SELECT id FROM locations WHERE deleted_at IS NULL AND parent_key=:parent AND name=:name'.($except===null?'':' AND id<>:except').' LIMIT 1 FOR UPDATE';$st=$db->prepare($sql);$params=[':parent'=>$parent??0,':name'=>$name];if($except!==null)$params[':except']=$except;$st->execute($params);return$st->fetchColumn()!==false;}
function loc_sibling_available(PDO $db,string $name,?int $parent,?int $except):void{if(loc_sibling_collision($db,$name,$parent,$except))loc_error('duplicate_location','A live sibling already has that name.',409);}
function inv_location_container_type($v,string $type):?int{$id=loc_optional_id($v,'container_type_id');if($id!==null){if($type!=='storage_unit')loc_error('bad_request','container_type_id requires a storage_unit.',400);if(!loc_live_row(Sor::db(),'container_types',$id))loc_error('not_found','Container type not found.',404);}return$id;}
function inv_location_project($v,string $type):?int{$id=loc_optional_id($v,'assigned_project_id');if($id!==null){if($type!=='storage_unit')loc_error('bad_request','assigned_project_id requires a storage_unit.',400);if(!loc_live_row(Sor::db(),'projects',$id))loc_error('not_found','Project not found.',404);}return$id;}
function loc_audit(PDO $db,string $table,int $id,string $action,?array $old,?array $new,string $note,int $source):void{$st=$db->prepare('INSERT INTO audit_log(table_name,row_id,action,actor_label,source_id,asserted_kind,old_values,new_values,note) VALUES(:table,:id,:action,:actor,:source,\'human\',:old,:new,:note)');$st->execute([':table'=>$table,':id'=>$id,':action'=>$action,':actor'=>'dashboard:'.Operator::name(),':source'=>$source,':old'=>$old===null?null:json_encode($old,JSON_THROW_ON_ERROR),':new'=>$new===null?null:json_encode($new,JSON_THROW_ON_ERROR),':note'=>$note]);}
function loc_type_row(array $r):array{foreach(['id']as$k)$r[$k]=(int)$r[$k];return$r;}
function loc_discover_fks(PDO $db):array{$st=$db->prepare("SELECT TABLE_NAME table_name,COLUMN_NAME column_name,CONSTRAINT_NAME constraint_name FROM information_schema.KEY_COLUMN_USAGE WHERE TABLE_SCHEMA=DATABASE() AND REFERENCED_TABLE_SCHEMA=DATABASE() AND REFERENCED_TABLE_NAME='locations' ORDER BY TABLE_NAME,COLUMN_NAME");$st->execute();return$st->fetchAll();}
function loc_fk_supported(array $fk):bool{return in_array($fk['table_name'].'.'.$fk['column_name'],['locations.parent_id','hardware_units.location_id','entities.location_id','power_circuits.location_id','projects.tray_location_id'],true);}
function loc_merge_counts(PDO $db,int $id):array{$counts=[];foreach(loc_discover_fks($db)as$fk){$t=$fk['table_name'];$c=$fk['column_name'];$st=$db->prepare('SELECT COUNT(*) FROM `'.str_replace('`','``',$t).'` WHERE `'.str_replace('`','``',$c).'`=:id');$st->execute([':id'=>$id]);$counts[$t.'.'.$c]=(int)$st->fetchColumn();}$polymorphic=["barcodes"=>"SELECT COUNT(*) FROM barcodes WHERE subject_table='locations' AND subject_id=:id AND deleted_at IS NULL","barcode_scans"=>"SELECT COUNT(*) FROM barcode_scans WHERE subject_table='locations' AND subject_id=:id","external_refs"=>"SELECT COUNT(*) FROM external_refs WHERE subject_table='locations' AND subject_id=:id"];$counts['barcodes']= (int)loc_scalar($db,$polymorphic['barcodes'],$id);$counts['barcode_scans']=(int)loc_scalar($db,$polymorphic['barcode_scans'],$id);$counts['external_refs']=(int)loc_scalar($db,$polymorphic['external_refs'],$id);return$counts;}
function loc_lock_merge_refs(PDO $db,int $id,array $fks):void{foreach($fks as$fk){$table=(string)$fk['table_name'];$column=(string)$fk['column_name'];$pk=loc_primary_key($db,$table);if($pk===null)continue;$st=$db->prepare('SELECT `'.str_replace('`','``',$pk).'` FROM `'.str_replace('`','``',$table).'` WHERE `'.str_replace('`','``',$column).'`=:id ORDER BY `'.str_replace('`','``',$pk).'` FOR UPDATE');$st->execute([':id'=>$id]);$st->fetchAll(PDO::FETCH_COLUMN);}foreach(["SELECT id FROM barcodes WHERE subject_table='locations' AND subject_id=:id FOR UPDATE","SELECT id FROM barcode_scans WHERE subject_table='locations' AND subject_id=:id FOR UPDATE","SELECT id FROM external_refs WHERE subject_table='locations' AND subject_id=:id FOR UPDATE"]as$sql){$st=$db->prepare($sql);$st->execute([':id'=>$id]);$st->fetchAll(PDO::FETCH_COLUMN);}}
function loc_scalar(PDO $db,string $sql,int $id){$st=$db->prepare($sql);$st->execute([':id'=>$id]);return$st->fetchColumn();}
function loc_merge_fingerprint(PDO $db,array $source,array $target,array $counts):string{$refs=[];foreach(loc_discover_fks($db)as$fk){$t=$fk['table_name'];$c=$fk['column_name'];$pk=loc_primary_key($db,$t);if($pk===null)continue;$st=$db->prepare('SELECT `'.str_replace('`','``',$pk).'` FROM `'.str_replace('`','``',$t).'` WHERE `'.str_replace('`','``',$c).'`=:id ORDER BY `'.str_replace('`','``',$pk)."` LIMIT 10001");$st->execute([':id'=>$source['id']]);$refs[$t.'.'.$c]=array_map('intval',$st->fetchAll(PDO::FETCH_COLUMN));}foreach(['barcodes','barcode_scans','external_refs']as$table){$sql=$table==='barcodes'?"SELECT id,code FROM barcodes WHERE subject_table='locations' AND subject_id=:id AND deleted_at IS NULL ORDER BY id":"SELECT id FROM `$table` WHERE subject_table='locations' AND subject_id=:id ORDER BY id";$st=$db->prepare($sql);$st->execute([':id'=>$source['id']]);$refs[$table]=$st->fetchAll();}$data=['source'=>[(int)$source['id'],$source['updated_at'],$source['loc_type'],$source['parent_id'],$source['assigned_project_id']],'target'=>[(int)$target['id'],$target['updated_at'],$target['loc_type'],$target['parent_id'],$target['assigned_project_id']],'counts'=>$counts,'refs'=>$refs];return hash('sha256',json_encode($data,JSON_THROW_ON_ERROR));}
function loc_primary_key(PDO $db,string $table):?string{$st=$db->prepare("SELECT COLUMN_NAME FROM information_schema.KEY_COLUMN_USAGE WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME=:t AND CONSTRAINT_NAME='PRIMARY' ORDER BY ORDINAL_POSITION LIMIT 1");$st->execute([':t'=>$table]);$v=$st->fetchColumn();return$v===false?null:(string)$v;}
function loc_public_location(array $r):array{return['id'=>(int)$r['id'],'parent_id'=>$r['parent_id']===null?null:(int)$r['parent_id'],'name'=>$r['name'],'loc_type'=>$r['loc_type'],'code'=>$r['code']??null,'assigned_project_id'=>$r['assigned_project_id']===null?null:(int)$r['assigned_project_id']];}
function loc_completed_merge(PDO $db,int $source,int $target):bool{$st=$db->prepare("SELECT id FROM audit_log WHERE table_name='locations' AND row_id=:id AND action='soft_delete' AND JSON_UNQUOTE(JSON_EXTRACT(new_values,'$.merge_target_id'))=:target ORDER BY id DESC LIMIT 1");$st->execute([':id'=>$source,':target'=>(string)$target]);return$st->fetchColumn()!==false;}
