# Write a program

Create a file called `hello.tr` and start typing:

```turf
import io;

fn main(): int {
    io::printline("Hello, Turf!");
    return 0;
}
```

You do not have to type all of it:

- type `main` and press **Tab** for the whole `fn main` skeleton;
- type `io::` and the module's functions appear, with their signatures;
- type `printline(` and the parameter you are on is highlighted (signature help);
- **Ctrl+click** a name to go to its definition, hover for its type.

Errors are underlined as you type, with the compiler's own explanation.
