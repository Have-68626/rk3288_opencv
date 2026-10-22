"""用生产CMake复制块和合成模型验证开关，不读取本机模型。"""
from pathlib import Path
import re
import shutil
import sys
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from testing.runner import Step, execute_step

ROOT = Path(__file__).resolve().parents[2]


def copy_fixture(source, directory, enabled, target='win_local_service'):
    directory.mkdir(parents=True)
    (directory / 'models').mkdir()
    (directory / 'models/synthetic-model-marker.txt').write_text('public-synthetic-model', encoding='utf-8')
    option = re.search(r'^option\(RK_COPY_DEV_MODELS[^\n]+$', source, re.M)
    blocks = list(re.finditer(r'^    if\((?:RK_COPY_DEV_MODELS AND )?EXISTS "\$\{CMAKE_CURRENT_SOURCE_DIR\}/models"\).*?^    endif\(\)', source, re.M | re.S))
    block = next((item for item in blocks if 'TARGET ' + target + ' POST_BUILD' in item.group(0)), None)
    if not block:
        raise ValueError('PRODUCTION_COPY_BLOCK_MISSING')
    cmake = ('cmake_minimum_required(VERSION 3.22)\nproject(CopyFixture LANGUAGES NONE)\n'
             + (option.group(0) + '\n' if option else '')
             + 'add_custom_target(' + target + ' ALL)\n'
             + block.group(0).replace('$<TARGET_FILE_DIR:' + target + '>', '${CMAKE_BINARY_DIR}/output') + '\n')
    (directory / 'CMakeLists.txt').write_text(cmake, encoding='utf-8')
    build = directory / 'build'
    command = [shutil.which('cmake'), '-S', str(directory), '-B', str(build), '-G', 'Ninja']
    if enabled is not None:
        command.append('-DRK_COPY_DEV_MODELS=' + ('ON' if enabled else 'OFF'))
    for name, args in [('configure', command), ('copy', [shutil.which('cmake'), '--build', str(build)])]:
        result = execute_step(Step(name, args, directory, timeout=90), directory / (name + '-report'))
        assert result['status'] == 'passed'
    return (build / 'output/models/synthetic-model-marker.txt').is_file()


@pytest.mark.skipif(sys.platform != 'win32' or not shutil.which('cmake') or not shutil.which('ninja'), reason='需要本地Windows CMake和Ninja')
@pytest.mark.parametrize('enabled,expected', [(None, True), (True, True), (False, False)])
@pytest.mark.parametrize('target', ['win_local_service', 'win_camera_face_recognition'])
def test_production_model_copy_switch(tmp_path, enabled, expected, target):
    source = (ROOT / 'cmake/windows.cmake').read_text(encoding='utf-8')
    assert copy_fixture(source, tmp_path / 'fixture', enabled, target) == expected
