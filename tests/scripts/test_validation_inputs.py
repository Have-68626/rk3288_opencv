"""资产身份校验不能冒充精度、设备加载或性能验收。"""
from pathlib import Path
import hashlib
import sys
import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'scripts'))


def description(tmp_path):
    model=tmp_path/'model.param';model.write_text('synthetic',encoding='utf-8')
    binary=tmp_path/'model.bin';binary.write_bytes(b'')
    return dict(schema_version=1,scope='synthetic',
        files={'model_param':dict(path='model.param',sha256=hashlib.sha256(model.read_bytes()).hexdigest()),
               'model_bin':dict(path='model.bin',sha256=hashlib.sha256(binary.read_bytes()).hexdigest())},
        source='自造公开夹具',license_reference='自造无第三方权重',
        parent_sha256=hashlib.sha256(b'generated').hexdigest(),
        preprocessing=dict(channel_order='RGB',mean=[127.5]*3,norm=[1/127.5]*3,
                           input_shape=[1,3,112,112],input_node='in0',output_node='out0',embedding_dim=512),
        measurement=dict(samples=1,warmup=0,steady_samples=1))


def test_input_hash_match_does_not_certify_accuracy_or_device(tmp_path):
    from testing.validation import inspect_inputs
    result=inspect_inputs(description(tmp_path),tmp_path)
    assert result['preflight_status']=='passed'
    assert result['accuracy']=='unverified' and result['device']=='not_run'
    assert result['performance']=='not_run'
    assert result['exit_code']==77


@pytest.mark.parametrize('change', ['missing','digest','preprocessing','provenance','media'])
def test_missing_or_ambiguous_inputs_block(tmp_path,change):
    from testing.validation import inspect_inputs
    data=description(tmp_path)
    if change=='missing':(tmp_path/'model.param').unlink()
    if change=='digest':data['files']['model_param']['sha256']='0'*64
    if change=='preprocessing':data['preprocessing']['norm']=[float('nan')]*3
    if change=='provenance':del data['parent_sha256']
    if change=='media':data['files']['model_param']['path']='private.jpg'
    assert inspect_inputs(data,tmp_path)['preflight_status']=='blocked'


def test_local_assets_require_explicit_opt_in(tmp_path):
    from testing.validation import inspect_inputs
    data=description(tmp_path);data['scope']='local-assets'
    assert inspect_inputs(data,tmp_path)['preflight_status']=='blocked'
    assert inspect_inputs(data,tmp_path,allow_local=True)['preflight_status']=='passed'
