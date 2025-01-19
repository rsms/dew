Dew is a little programming language which targets the Lua virtual machine.


## Language

### Examples

```
1 + (2 + 3) * (4 - 5)
fun f
fun f(x, y int)
fun f(x, y int) int
fun f(x, y int) int, int
fun f(x, y int) (int, int)
fun ♥︎(x, y, z int) int, int {
	return x + y*z
}
```

### Operators

	+    &     +=    &=     &&    ==    !=    (    )
	-    |     -=    |=     ||    <     <=    [    ]
	*    ^     *=    ^=     <-    >     >=    {    }
	/    <<    /=    <<=    ++    =     :=    ,    ;
	%    >>    %=    >>=    --    !     ...   .    :
	~

#### Operator precedence

Binary operators of the same precedence associate from left to right.
E.g. `x / y * z` is the same as `(x / y) * z`.

	7  .
	6  ++  --  +  -  !  ~  *  &  ?
	5  *  /  %  <<  >>  &
	4  +  -  |  ^
	3  ==  !=  <  <=  >  >=
	2  &&
	1  ||
	0  ,


## Building dew

```
make && o.*/dew examples/hello.dew
```

Requirements:

- C17 compiler (tested with clang 17)
- GNU Make
- curl for wasm build

Various ways to build:

- `make` — release build for host system
- `make DEBUG=1` — debug build for host system
- `make TARGET=web` — release build for web platform
- `make TARGET=web DEBUG=1` — debug build for web platform
- `make test` — build & run tests
