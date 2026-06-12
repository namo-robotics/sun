# Example: Transitive Moons

This example demonstrates a transitive moon dependency chain: 

```main
graph LR
    main[main.sun] --> moon1[moon1.moon]
    main --> stdlib[stdlib.moon]
    moon1 --> moon2[moon2.moon]
    moon2 --> moon3[moon3.moon]
```

`main` can see symbols defined in `moon1` but can't see
symbols in `moon2` or `moon3`. 

The compiled `moon1.moon` file contains the bitcode of `moon2` and `moon3`.