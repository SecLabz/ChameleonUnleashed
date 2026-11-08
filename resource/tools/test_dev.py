"""Verify firmware versions, package selection and flash failure boundaries."""

from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
import io
import json
import os
import shutil
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from project_version import read_version


loader = SourceFileLoader('project_dev', str(Path(__file__).resolve().parents[2] / 'dev'))
dev = module_from_spec(spec_from_loader(loader.name, loader))
loader.exec_module(dev)


class DevelopmentTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        lock = self.root / 'software/web/build-lock.json'
        lock.parent.mkdir(parents=True)
        lock.write_text(json.dumps({'rust': '1.93.1', 'shared': {'revision': 'locked'}}))
        self.app = self.root / 'firmware/objects/chameleon-unleashed-app.zip'
        self.app.parent.mkdir(parents=True)
        self.app.write_bytes(b'previous firmware')
        self.full = self.app.with_name('chameleon-unleashed-full.zip')
        cdn = self.root / '.cache/web/chameleon-unleashed-cdn.zip'
        cdn.parent.mkdir(parents=True)
        cdn.write_bytes(b'matching CDN')
        self.patch(dev, 'ROOT', new=self.root)
        self.run = self.patch(dev, 'run')
        self.patch(dev.shutil, 'which', return_value='/tools/tool')
        self.patch(dev.sys, 'stdout', new=io.StringIO())
        self.patch(dev.sys, 'stderr', new=io.StringIO())

    def patch(self, target, name, **kwargs):
        patch = mock.patch.object(target, name, **kwargs)
        self.addCleanup(patch.stop)
        return patch.start()

    def test_build_failure_never_flashes_an_existing_zip(self):
        for failed in ['build_web.py', 'package_cdn.py', 'build.sh']:
            def run(*command, **kwargs):
                if any(str(value).endswith(failed) for value in command):
                    raise subprocess.CalledProcessError(1, command)

            self.run.side_effect = run
            with self.subTest(failed=failed):
                self.run.reset_mock()
                with self.assertRaises(SystemExit) as error:
                    dev.main(['flash', '--shared-source', str(self.root)])

                self.assertEqual(error.exception.code, 1)
                self.assertFalse(any(call.args[:2] == ('cargo', 'run')
                                     for call in self.run.call_args_list))
                self.assertEqual(self.app.read_bytes(), b'previous firmware')

    def test_flash_selects_application_or_full_after_a_successful_build(self):
        install = self.app.with_name('chameleon-unleashed-install.zip')
        for options, package in [([], self.app), (['--full'], self.full), (['--install-recovery'], install)]:
            with self.subTest(options=options):
                self.run.reset_mock()
                dev.main(['flash', '--shared-source', str(self.root), *options])

                calls = [call for call in self.run.call_args_list if call.args[:2] == ('cargo', 'run')]
                self.assertEqual(len(calls), 1)
                self.assertEqual(calls[0].args, (
                    'cargo', 'run', '--manifest-path', self.root / 'software/Cargo.toml',
                    '--locked', '--release', '-p', 'chameleon-cli', '--', 'flash', package, '--yes',
                ))
                self.assertTrue(any(call.args[0] == 'bash' for call in self.run.call_args_list[:-1]))

    def test_cli_build_only_needs_native_build_tools_and_uses_locked_dependencies(self):
        with mock.patch.object(dev.shutil, 'which',
                               side_effect=lambda name: '/tools/' + name if name in {'git', 'cargo'} else None):
            dev.main(['build', 'cli'])

        self.run.assert_called_once()
        command = self.run.call_args
        self.assertEqual(command.args, (
            'cargo', 'build', '--manifest-path', self.root / 'software/Cargo.toml',
            '--locked', '--release', '-p', 'chameleon-cli',
        ))

    def test_firmware_build_and_flash_only_clean_when_requested(self):
        for command in ['build', 'flash']:
            for options in [[], ['--clean']]:
                with self.subTest(command=command, options=options):
                    self.run.reset_mock()
                    dev.main([command, '--shared-source', str(self.root), *options])

                    builds = [call for call in self.run.call_args_list if call.args[0] == 'bash']
                    self.assertEqual(len(builds), 1)
                    self.assertEqual(builds[0].args, (
                        'bash', self.root / 'firmware/build.sh', *options,
                    ))

    def test_cli_rejects_firmware_cleanup_before_running_tools(self):
        with self.assertRaises(SystemExit) as error:
            dev.main(['build', 'cli', '--clean'])

        self.assertEqual(error.exception.code, 2)
        self.run.assert_not_called()

    def test_fork_resource_url_is_passed_to_web_builds(self):
        base = 'https://alice.github.io/ChameleonUnleashed/'
        for command in ['build', 'flash', 'cdn']:
            with self.subTest(command=command):
                self.run.reset_mock()
                dev.main([command, '--shared-source', str(self.root), '--asset-base', base])

                builds = [call for call in self.run.call_args_list
                          if str(call.args[1]).endswith('build_web.py')]
                self.assertEqual(len(builds), 1)
                options = builds[0].args
                self.assertEqual(options[options.index('--asset-base') + 1], base)

    def test_cli_rejects_a_resource_url_before_running_tools(self):
        with self.assertRaises(SystemExit) as error:
            dev.main(['build', 'cli', '--asset-base', 'https://cdn.example/'])

        self.assertEqual(error.exception.code, 2)
        self.run.assert_not_called()

    def test_local_server_prepares_missing_resources_without_packaging(self):
        with mock.patch('serve_web.serve') as serve, mock.patch('serve_web.resources_ready', return_value=False):
            dev.main(['serve', '--shared-source', str(self.root)])

        builds = [call for call in self.run.call_args_list
                  if str(call.args[1]).endswith('build_web.py')]
        self.assertEqual(len(builds), 1)
        options = builds[0].args
        self.assertEqual(options[options.index('--asset-base') + 1], '/')
        self.assertIn('--resources-only', options)
        self.assertEqual(len(self.run.call_args_list), 1)
        serve.assert_called_once_with(
            self.root / 'software/web', self.root / '.cache/web', self.root.resolve(),
            'http://192.168.16.1', 8080,
        )

    def test_local_server_reuses_prepared_resources_without_a_build(self):
        with mock.patch('serve_web.serve') as serve, mock.patch('serve_web.resources_ready', return_value=True):
            dev.main(['serve', '--shared-source', str(self.root), '--device', 'http://device', '--port', '8090'])

        self.run.assert_not_called()
        serve.assert_called_once_with(
            self.root / 'software/web', self.root / '.cache/web', self.root.resolve(), 'http://device', 8090,
        )

    def test_local_preparation_failure_never_starts_the_server(self):
        self.run.side_effect = subprocess.CalledProcessError(1, 'prepare')
        with mock.patch('serve_web.serve') as serve, mock.patch('serve_web.resources_ready', return_value=False):
            with self.assertRaises(SystemExit) as error:
                dev.main(['serve', '--shared-source', str(self.root)])

        self.assertEqual(error.exception.code, 1)
        serve.assert_not_called()

    def test_ble_server_reuses_local_resources_without_usb_or_packaging(self):
        with mock.patch('serve_web.serve') as serve, mock.patch('serve_web.resources_ready', return_value=True):
            dev.main(['serve', 'ble', '--shared-source', str(self.root), '--port', '8091'])

        self.run.assert_not_called()
        serve.assert_called_once_with(
            self.root / 'software/web', self.root / '.cache/web', self.root.resolve(), None, 8091,
        )

    def test_native_flash_failure_is_reported_without_retry(self):
        def run(*command, **kwargs):
            if command[:2] == ('cargo', 'run'):
                raise subprocess.CalledProcessError(1, command)

        self.run.side_effect = run
        with self.assertRaises(SystemExit) as error:
            dev.main(['flash', '--shared-source', str(self.root)])

        self.assertEqual(error.exception.code, 1)
        self.assertEqual(sum(call.args[:2] == ('cargo', 'run') for call in self.run.call_args_list), 1)


class FirmwareVersionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        definitions = Path(__file__).resolve().parents[2] / 'firmware/Makefile.defs'
        (self.root / 'Makefile').write_text(
            f'include {definitions}\n'
            'version:\n'
            '\t@printf "%s\\n" "$(APP_FW_SEMVER)" "$(APP_FW_VER_MAJOR)" '
            '"$(APP_FW_VER_MINOR)" "$(GIT_VERSION)"\n'
        )

    def version(self, *options, env=None):
        result = subprocess.run(['make', '--no-print-directory', 'version', *options],
                                cwd=self.root, env=env, capture_output=True, text=True, check=True)
        self.assertEqual(result.stderr, '')
        return result.stdout.splitlines()

    def test_firmware_version_needs_neither_python_nor_git(self):
        tools = self.root / 'bin'
        tools.mkdir()
        for name in ['make', 'awk']:
            (tools / name).symlink_to(shutil.which(name))
        version = read_version()
        self.assertEqual(self.version(env={**os.environ, 'PATH': str(tools)}),
                         [version, *version.split('.')[:2], 'v' + version])

    def test_source_archive_has_a_version_without_git(self):
        version, major, minor, description = self.version()
        self.assertEqual(version, read_version())
        self.assertTrue(version.startswith(f'{int(major)}.{int(minor)}.'))
        self.assertEqual(description, 'v' + version)
        self.assertEqual(self.version('APP_FW_SEMVER=3.7.1'), ['3.7.1', '3', '7', 'v3.7.1'])

    def test_new_history_and_tags_do_not_change_the_firmware_version(self):
        expected = self.version()[:3]
        for command in [
            ['git', 'init', '-q'],
            ['git', '-c', 'user.name=Test', '-c', 'user.email=test@example.invalid',
             '-c', 'commit.gpgsign=false', 'commit', '-qm', 'Initial snapshot', '--allow-empty'],
        ]:
            subprocess.run(command, cwd=self.root, check=True, capture_output=True)

        version = self.version()
        self.assertEqual(version[:3], expected)
        revision = subprocess.check_output(['git', 'rev-parse', '--short=7', 'HEAD'],
                                           cwd=self.root, text=True).strip()
        self.assertEqual(version[3], revision)

        subprocess.run(['git', '-c', 'tag.gpgsign=false', 'tag', 'v9.8.7'], cwd=self.root, check=True)
        version = self.version()
        self.assertEqual(version[:3], expected)
        self.assertEqual(version[3], 'v9.8.7')


if __name__ == '__main__':
    unittest.main()
