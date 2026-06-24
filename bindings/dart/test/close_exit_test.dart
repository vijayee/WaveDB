// test/close_exit_test.dart
//
// Regression test: Dart process should exit cleanly after closing all
// WaveDB instances. Before the fix, the static NativeCallable.listener
// callbacks were never closed, keeping the Dart VM's internal
// "DartWorker" threads alive and preventing process exit.
//
// This test spawns a subprocess that creates a WaveDB, closes it, and
// prints "ready-to-exit". If the process exits within 30 seconds, the
// test passes. If it hangs (because the NativeCallables weren't closed),
// the test fails.

import 'dart:async';
import 'dart:io';

import 'package:path/path.dart' as p;
import 'package:test/test.dart';

final _packageConfig =
    p.absolute('.dart_tool', 'package_config.json');

String _writeScript(String content) {
  final script = File('.dart_tool/temp_close_exit_script.dart');
  script.writeAsStringSync(content);
  return script.path;
}

const _closeAndExitScript = r'''
import 'dart:io';
import 'package:wavedb/wavedb.dart';

Future<void> main() async {
  final base = Directory.systemTemp.createTempSync('wavedb_close_exit_');
  final db = WaveDB('${base.path}/db',
      config: WaveDBConfig(enablePersist: false, walSyncMode: 'async'));
  await db.put('k', 'v');
  db.close();
  await base.delete(recursive: true);
  print('ready-to-exit');
}
''';

void main() {
  test('Dart process exits cleanly after closing all WaveDB instances', () {
    final scriptPath = _writeScript(_closeAndExitScript);
    final result = Process.runSync(
      'dart',
      ['run', '--packages=$_packageConfig', scriptPath],
      stdoutEncoding: systemEncoding,
      stderrEncoding: systemEncoding,
    );

    final filteredStderr = result.stderr
        .split('\n')
        .where((l) =>
            !l.contains('INFO') &&
            !l.contains('WARN') &&
            !l.contains('ERROR ') &&
            l.trim().isNotEmpty)
        .join('\n');

    expect(result.exitCode, 0,
        reason: 'Process should exit cleanly.\n'
            'stdout: ${result.stdout}\n'
            'stderr (filtered): $filteredStderr');
    expect(result.stdout.contains('ready-to-exit'), isTrue,
        reason: 'Process should have completed its work');
  }, timeout: Timeout(Duration(seconds: 30)));

  test('WaveDB.shutdown() is callable and idempotent', () {
    final scriptPath = _writeScript(r'''
import 'package:wavedb/wavedb.dart';

void main() {
  WaveDB.shutdown();
  WaveDB.shutdown();  // idempotent
  print('ok');
}
''');
    final result = Process.runSync(
      'dart',
      ['run', '--packages=$_packageConfig', scriptPath],
      stdoutEncoding: systemEncoding,
      stderrEncoding: systemEncoding,
    );
    expect(result.exitCode, 0);
    expect(result.stdout.contains('ok'), isTrue);
  }, timeout: Timeout(Duration(seconds: 30)));
}