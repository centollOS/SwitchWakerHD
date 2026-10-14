"""Embedded-Python startup assembly; native SDL lifecycle performs GPU warming."""
import argparse
import importlib.util
import json
from pathlib import Path
import sys


def run(configuration):
    job = json.loads(Path(configuration).read_text())
    package = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(package / 'tools'))
    spec = importlib.util.spec_from_file_location('android_shader_prebuild', package / 'tools/installer/shader_prebuild.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    import shaderprep
    root = Path(job['cache_root'])
    root.mkdir(parents=True, exist_ok=True)
    helper.atomic_json(root / 'launcher.json', dict(schema=1, package=str(package),
        helper=str(package / 'tools/installer/shader_prebuild.py'),
        template=str(package / 'tools/shadercache/template.bin')))
    def build(game, output, template, progress=None):
        shaderprep.cmd_build(str(game), str(output), str(template), 0, True, progress, jobs=2, threads=True)
    result = helper.prepare(job['game'], package / 'tools/shadercache/template.bin', None,
        root, package, renderer='vulkan', builder=build, runtime_identity=job['library'],
        progress=lambda text, fraction=None: print('[shader preparation] ' + (
            text if fraction is None else '%d%% %s' % (100 * fraction, text)), flush=True))
    helper.atomic_json(Path(configuration).with_suffix('.result.json'), result)
    return result


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--configuration', required=True)
    return run(parser.parse_args(argv).configuration)
