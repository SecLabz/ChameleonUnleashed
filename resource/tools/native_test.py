"""Compile and run native C regression fixtures with sanitizers."""
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
    """Extract a complete production function for compilation with hardware stubs."""
    match = re.search(r'^(?:static\s+)?\w+\s+\*?\s*' + name + r'\([^;]*?\)\s*\{', source, re.M)
    if match is None:
        raise ValueError(f'Missing function: {name}')

    end = match.end()
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1

    return source[match.start():end]


def run_c(source):
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)
        (path / 'test.c').write_text(source)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                        '-fsanitize=address,undefined', str(path / 'test.c'),
                        '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True, timeout=10)
