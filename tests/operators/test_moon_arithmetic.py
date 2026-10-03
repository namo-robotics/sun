"""Check generic arithmetic hooks after serialization through a moon bundle."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    """Compile a reusable generic class, then exercise its hooks through JIT and AOT."""
    parser = argparse.ArgumentParser()
    parser.add_argument('--sun', required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    (root / 'tmp').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='operator-moon-', dir=root / 'tmp') as directory:
        path = Path(directory)
        library = path / 'arithmetic.sun'
        library.write_text('''/** Exports generic arithmetic for another compilation. */
public module arithmetic {
  /** Owns a numeric value. */
  public class Box<T: _Numeric> {
    public var value: T;
    /** Initializes the stored value. */
    init(value: T) { this.value = value; }
    /** Returns a new sum without consuming its operands. */
    public const method __add__(other: const ref Box<T>) Box<T> {
      return Box<T>(this.value + other.value);
    }
    /** Returns a scalar product. */
    public const method __multiply__(other: T) T { return this.value * other; }
  }
  /** Instantiates an operator call inside a serialized generic body. */
  public function sum<T: _Numeric>(a: const ref Box<T>, b: const ref Box<T>) Box<T> {
    return a + b;
  }
}
''')
        consumer = path / 'consumer.sun'
        consumer.write_text('''manifest { libraries: ["arithmetic.moon"] }
using arithmetic;
/** Exercises imported hooks, temporary results, and generic function bodies. */
function main() i32 {
  var a = Box<i32>(10);
  var b = Box<i32>(11);
  var c = a + b;
  var d = sum<i32>(a, b);
  if (c * 2 != 42 or d.value != 21) { return 1; }
  return 0;
}
''')
        env = os.environ.copy()
        env['SUN_PATH'] = str(root)
        subprocess.run([args.sun, '--emit-moon', '-o', str(path / 'arithmetic.moon'), str(library)], check=True, env=env)
        common = [args.sun, '--lib-path', directory]
        subprocess.run(common + [str(consumer)], check=True, env=env)
        binary = str(path / 'consumer')
        subprocess.run(common + ['-c', '--dynamic', '-o', binary, str(consumer)], check=True, env=env)
        subprocess.run([binary], check=True, env=env)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
