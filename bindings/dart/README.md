# WaveDB Dart Bindings

Dart FFI bindings for WaveDB, a hierarchical key-value database.

## Installation

Add to your `pubspec.yaml`:

```yaml
dependencies:
  wavedb: ^0.1.0
```

## Prerequisites

You must have the WaveDB C library available:
- Linux: `libwavedb.so`
- macOS: `libwavedb.dylib`
- Windows: `wavedb.dll`

Build from source in the WaveDB project.

## Quick Start

```dart
import 'package:wavedb/wavedb.dart';

void main() async {
  final db = WaveDB('/path/to/database');

  await db.put('users/alice/name', 'Alice');
  final name = await db.get('users/alice/name');
  print(name); // 'Alice'

  db.close();
}
```

## Testing

Tests require the native WaveDB library (`libwavedb.so`, `libwavedb.dylib`, or `wavedb.dll`).

Build the library first:
```bash
cd /path/to/WaveDB
mkdir build && cd build
cmake ..
make
```

Then run tests:
```bash
cd bindings/dart
export LD_LIBRARY_PATH=/path/to/WaveDB/build:$LD_LIBRARY_PATH  # Linux
# or: export DYLD_LIBRARY_PATH=... on macOS
dart test
```

## License

GNU General Public License v3.0 or later
