# Mòdul Python per a depurar

Aquest mòdul fa una implementació bàsica del simulador utilitzant
Python, SDL1.2 i OpenGL amb l'objectiu de poder depurar el simulador:

- Pantalla amb resolució original
- Controls "hardcodejats":
  - UP: W
  - DOWN: S
  - LEFT: A
  - RIGHT: D
  - TRIANGLE: I
  - CIRCLE: O
  - SQUARE: K
  - CROSS: L
  - L1: U
  - R1: P
  - L2: Q
  - R2: E
  - START: Espai
  - SELECT: Retorn
- No es desa l'estat
- **CTRL-Q** per a eixir.
- **CTRL-R** per a reiniciar.

Per a instal·lar el mòdul

```
pip install .
```

Un exemple bàsic d'ús es pot trobar en **exemple.py**:
```
python3 exemple.py BIOS CDIMG
```

En la carpeta **debug** hi ha un script utilitzat per a depurar.
