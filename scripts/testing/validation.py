"""离线输入预检；来源许可仅记录声明，真实验收始终独立。"""
import hashlib
import math
from pathlib import Path
import re


def inspect_inputs(data,base,allow_local=False):
    result=dict(preflight_status='blocked',accuracy='unverified',device='not_run',
                performance='not_run',exit_code=2,files={},
                provenance_status='declared_not_independently_verified')
    try:
        if data['schema_version']!=1 or data['scope'] not in ('synthetic','local-assets'):
            raise ValueError()
        if data['scope']=='local-assets' and not allow_local:
            raise ValueError()
        for field in ('source','license_reference'):
            if not isinstance(data[field],str) or not data[field].strip():raise ValueError()
        if not re.fullmatch('[a-f0-9]{64}',data['parent_sha256']):raise ValueError()
        preprocess=data['preprocessing']
        if preprocess['channel_order'] not in ('RGB','BGR'):raise ValueError()
        for key in ('mean','norm'):
            values=preprocess[key]
            if len(values)!=3 or any(isinstance(v,bool) or not isinstance(v,(int,float)) or not math.isfinite(v) for v in values):raise ValueError()
        if any(v<=0 for v in preprocess['norm']):raise ValueError()
        shape=preprocess['input_shape']
        if len(shape)!=4 or shape[:2]!=[1,3] or any(type(v)!=int or v<=0 for v in shape):raise ValueError()
        if type(preprocess['embedding_dim'])!=int or preprocess['embedding_dim']<=0:raise ValueError()
        if not all(isinstance(preprocess[k],str) and preprocess[k] for k in ('input_node','output_node')):raise ValueError()
        measurement=data['measurement']
        for key in ('samples','warmup','steady_samples'):
            if type(measurement[key])!=int or measurement[key]<(0 if key=='warmup' else 1):raise ValueError()
        if measurement['steady_samples']>measurement['samples']:raise ValueError()
        if not isinstance(data['files'],dict) or not {'model_param','model_bin'}<=data['files'].keys():raise ValueError()
        suffixes={'model_param':'.param','model_bin':'.bin','parent_model':'.onnx','apk':'.apk'}
        for role,item in data['files'].items():
            path=Path(base)/item['path']
            if role not in suffixes or path.suffix.lower()!=suffixes[role] or path.resolve()!=path.absolute() or not path.is_file():raise ValueError()
            if not re.fullmatch('[a-f0-9]{64}',item['sha256']):raise ValueError()
            digest=hashlib.sha256()
            with path.open('rb') as stream:
                while chunk:=stream.read(1024*1024):digest.update(chunk)
            if digest.hexdigest()!=item['sha256']:raise ValueError()
            result['files'][role]=dict(sha256=digest.hexdigest())
        selected_preprocess={key:preprocess[key] for key in ('channel_order','mean','norm','input_shape','input_node','output_node','embedding_dim')}
        selected_measurement={key:measurement[key] for key in ('samples','warmup','steady_samples')}
        result.update(preflight_status='passed',exit_code=77,scope=data['scope'],
                      preprocessing=selected_preprocess,measurement_plan=selected_measurement,
                      parent_sha256=data['parent_sha256'],source=data['source'],
                      license_reference=data['license_reference'],files_scope='declared_files_not_runtime')
    except (ValueError,KeyError,TypeError,OSError):
        result['reason']='VALIDATION_INPUTS_INCOMPLETE_OR_UNVERIFIED'
    return result
