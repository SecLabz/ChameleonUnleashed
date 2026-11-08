"""Serve the actual standalone BLE package beneath a repository-style prefix."""
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'resource/tools'))
from prepare_hosted_web import prepare


with tempfile.TemporaryDirectory(prefix='chameleon-hosted-tests-') as temporary:
    root = Path(temporary)
    prepare(ROOT / '.cache/web/assets/site', ROOT / '.cache/web/chameleon-unleashed-cdn.zip',
            root / 'project/ChameleonUnleashed')
    handler = partial(SimpleHTTPRequestHandler, directory=root)
    ThreadingHTTPServer(('127.0.0.1', 8085), handler).serve_forever()
