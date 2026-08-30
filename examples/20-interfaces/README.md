# Interfaces

Interfaces declare behaviour that classes can `implements`. A function taking
`ref Drawable` dispatches dynamically to the concrete type at runtime, so
`render` draws a `Circle` or a `Square` without knowing which it holds.

## Build and run

```bash
./build.sh
./main
```
