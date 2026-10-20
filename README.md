# Sistema de Facturación – Representaciones "Chapina", C.A.

Proyecto final del curso de **Algoritmos** – Universidad Mariano Gálvez de Guatemala.

**Autor:** Werner Fernando Cifuentes Barillas (carnet 0901-25-23316)

Aplicación de consola en **C++17** con base de datos **SQLite** para una empresa distribuidora de productos de limpieza al por mayor. Permite:

- Registrar **empleados** (Vendedores y Repartidores) en una sola tabla general, diferenciados por tipo.
- Registrar **clientes**, cada uno asignado a un vendedor.
- Registrar **productos** y controlar el **inventario**.
- Emitir **facturas** al contado o al crédito, con IVA del 12 % y descuento automático de inventario.
- Registrar **pagos** completos o parciales, con control de saldo.
- Emitir 6 **reportes**: Clientes, Vendedores, Productos Vendidos, Ventas del Día, Pagos del Día e Inventario Existente.

## Estructura del repositorio

| Carpeta / archivo | Contenido |
|---|---|
| `src/main.cpp` | Código fuente completo (un solo archivo) |
| `db/chapina_demo.db` | Base de datos de demostración con datos de ejemplo |
| `docs/` | Documentación del proyecto (PDF final y diagrama de Gantt) |
| `pruebas/resultados_pruebas.json` | Resultado de los 90 casos de prueba |
| `Makefile` | Atajos para compilar y ejecutar |

## Requisitos

- Compilador **g++** con soporte de C++17.
- Biblioteca **SQLite3** con sus encabezados (`sqlite3.h`).

### Linux / WSL (Ubuntu)

```bash
sudo apt update
sudo apt install g++ libsqlite3-dev
```

### Windows (MSYS2)

1. Instalar [MSYS2](https://www.msys2.org/) y abrir la terminal **MSYS2 UCRT64**.
2. Instalar las herramientas:

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-sqlite3
```

## Compilar

```bash
g++ -std=c++17 -Wall -Wextra -o chapina src/main.cpp -lsqlite3
```

(o simplemente `make`). Compila sin advertencias.

## Ejecutar

Con la base de datos de demostración:

```bash
./chapina db/chapina_demo.db
```

Con una base de datos nueva y vacía (se crea sola al iniciar):

```bash
./chapina
```

## Usuarios de la demostración

| Rol | Usuario | Clave |
|---|---|---|
| Administrador | `admin` | `admin123` |
| Vendedor | `V001` (Carlos Méndez López) | su DPI: `2456789010101` |
| Vendedor | `V002` (Ana Lucía Ruiz) | su DPI: `2987654320101` |

Los Repartidores (por ejemplo `R001`) no inician sesión. El Administrador ve todos los módulos; cada Vendedor solo ve Clientes, Facturación y Pagos, limitados a su propia cartera.

## Aspectos técnicos

- Consultas SQL **parametrizadas** (protección contra inyección SQL).
- La factura se guarda dentro de una **transacción**: si algo falla, se revierte todo (incluido el inventario).
- Llaves foráneas activadas y restricciones `CHECK` y `UNIQUE` en la base de datos.
- Importes redondeados a 2 decimales; IVA = 12 %.

## Pruebas

Se ejecutaron **90 casos de prueba**, todos aprobados, con cobertura del 96.3 % de las líneas y sin errores de memoria (AddressSanitizer y UBSan). El detalle está en el Entregable 8 dentro de `docs/` y en `pruebas/resultados_pruebas.json`.
