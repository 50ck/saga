# H1: Titulo principal

## H2: Subtitulo

### H3: Seccion

#### H4: Subseccion

##### H5: Detalle

###### H6: Detalle minimo

---

**Negrita con doble asterisco**

*Italic con asterisco*

___Negrita+italic con tres guiones bajos___

__Negrita con doble guion bajo__

_Negrita+italic con guion bajo_

~~Texto tachado~~

`inline code` con backticks

`` `backtick literal` `` dentro de code

~~**negrita tachada**~~

### Enlaces e imagenes

[texto de enlace](https://example.com "title")

[enlace con anchor](https://example.com#section)

[autolink sin paréntesis](https://example.com)

![alt text](https://example.com/img.png "caption")

### Code blocks

```python
def hola(mundo: str) -> str:
    return f"hola, {mundo}!"

# comentario
print(hola("mundo"))
```

```bash
#!/bin/sh
echo "esto es bash"
ls -la /tmp
```

```
sin language, se muestra como pre
```

### Blockquotes

> Cita de una linea

> Cita de varias lineas
> segunda linea
> tercera linea

> ### Encabezado dentro de cita
> con **negrita** y `code`

### Listas

#### Unordered

- item uno
- item dos
  - subitem a
  - subitem b
    - subsubitem
- item tres

#### Ordered

1. primero
2. segundo
3. tercero
   1. sub-ordenado a
   2. sub-ordenado b

#### Task list

- [x] tarea completada
- [ ] tarea pendiente
- [ ] otra pendiente
  - [ ] sub-tarea pendiente

#### Mixto

- [ ] a
- [x] b
  1. sub 1
  2. sub 2

### Tablas

| Columna A | Columna B | Columna C |
|-----------|:---------:|----------:|
| izquierda |  centro   | derecha   |
| a         |    b      |        c  |
| `code`    | **bold**  | ~~~strike~~~ |

### Horizontal rules

---

***

___

---

### Mixto en una frase

Esta frase tiene **negrita**, *italic*, ~~tachado~~, `code`, [enlace](https://example.com), > no, esto no va aqui, y un ![img](https://example.com/x.png).

### Numeros y simbolos

1. `const x = 42;`
2. **negrita** y *italic* en la misma linea
3. ~~~triple tilde~~~ no es estandar pero algunas renderizaciones lo aceptan

### Multilinea con line break

linea uno
linea dos con backslash\
linea tres con dos espacios   

### Fenced code con info string

```javascript {6} title="Ejemplo.js"
// line 1
// line 2
// line 3
// line 4
// line 5
const answer = 42; // <em>esta es la line resaltada</em>
```

### Nested blockquote con code

> ```python
> print("cita con code")
> ```

### Tabla con code y negrita

| Funcion | Uso | Ejemplo |
|---------|-----|---------|
| `echo` | imprimir | `echo "hola"` |
| **grep** | buscar | `grep -r "patron" .` |
| `ls` | listar | `ls -la` |

### Final

---

*fin del documento*
