// example/example.dart
import 'package:wavedb/wavedb.dart';

void main() async {
  // Open database
  final db = WaveDB('/tmp/my_database', delimiter: '/');

  try {
    // Sync operations
    print('=== Sync Operations ===');

    db.putSync('users/alice/name', 'Alice');
    db.putSync('users/alice/age', '30');
    db.putSync(['users', 'bob', 'name'], 'Bob');

    print('Alice: ${db.getSync('users/alice/name')}');
    print('Bob: ${db.getSync(['users', 'bob', 'name'])}');

    // Async operations
    print('\n=== Async Operations ===');

    await db.put('users/charlie/name', 'Charlie');
    final name = await db.get('users/charlie/name');
    print('Charlie: $name');

    // Batch operations
    print('\n=== Batch Operations ===');

    await db.batch([
      {'type': 'put', 'key': 'counter/a', 'value': '1'},
      {'type': 'put', 'key': 'counter/b', 'value': '2'},
      {'type': 'del', 'key': 'users/charlie/name'},
    ]);

    print('Counter A: ${await db.get('counter/a')}');
    print('Charlie: ${await db.get('users/charlie/name')}');

    // Object operations
    print('\n=== Object Operations ===');

    await db.putObject('products', {
      'apple': {'name': 'Apple', 'price': '1.99'},
      'banana': {'name': 'Banana', 'price': '0.99'},
    });

    print('Apple: ${await db.get('products/apple/name')}');
    print('Banana price: ${await db.get('products/banana/price')}');

    // Binary data
    print('\n=== Binary Data ===');

    final binary = [0x01, 0x02, 0x03, 0xFF];
    await db.put('binary/key', binary);
    final value = await db.get('binary/key');
    print('Binary value: $value');

    // Stream iteration (requires native scan API)
    print('\n=== Stream Iteration ===');

    try {
      await for (final entry in db.createReadStream(start: 'users/')) {
        print('Key: ${entry.key}, Value: ${entry.value}');
      }
    } on WaveDBException catch (e) {
      if (e.code == 'NOT_SUPPORTED') {
        print('Stream iteration requires database_scan API');
      } else {
        rethrow;
      }
    }
  } finally {
    // Always close the database
    db.close();
    print('\nDatabase closed.');
  }
}
