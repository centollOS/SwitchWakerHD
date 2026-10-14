"""Exercise dirty-source invalidation through CMake's actual build dependency graph."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class PipelineBuildIdentityTest(unittest.TestCase):
    def test_dirty_changes_reconfigure_without_git_or_game_inputs(self):
        helper = Path(__file__).resolve().parents[2] / 'cmake/PipelineRuntimeIdentity.cmake'
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'source'
            source.mkdir()
            shutil.copy(helper, source / helper.name)
            runtime = source / 'runtime/src/gfx/vulkan'
            runtime.mkdir(parents=True)
            state = runtime / 'state.cpp'
            state.write_text('public synthetic state version one\n')
            (source / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(identity NONE)
include(PipelineRuntimeIdentity.cmake)
wwhd_pipeline_runtime_identity("${CMAKE_SOURCE_DIR}" identity)
file(WRITE "${CMAKE_BINARY_DIR}/identity" "${identity}")
add_custom_target(check ALL COMMAND "${CMAKE_COMMAND}" -E true)
''')
            build = Path(directory) / 'build'

            def run(*args):
                subprocess.run(['cmake', *map(str, args)], check=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)

            run('-S', source, '-B', build)
            original = (build / 'identity').read_text()
            state.write_text('public synthetic state version two\n')
            run('--build', build)
            changed = (build / 'identity').read_text()
            self.assertNotEqual(original, changed)
            extra = runtime / 'new-state.h'
            extra.write_text('new public synthetic state\n')
            run('--build', build)
            added = (build / 'identity').read_text()
            self.assertNotEqual(changed, added)
            extra.unlink()
            run('--build', build)
            self.assertEqual(changed, (build / 'identity').read_text())
            # Relocating an unchanged source tree must not invalidate recipes.
            relocated = Path(directory) / 'relocated'
            shutil.copytree(source, relocated)
            run('-S', relocated, '-B', Path(directory) / 'other-build')
            self.assertEqual(changed, (Path(directory) / 'other-build/identity').read_text())


if __name__ == '__main__':
    unittest.main()
