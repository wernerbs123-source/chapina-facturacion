// ============================================================================
//  Sistema de Facturación - Representaciones "Chapina", C.A.
//  Autor : Werner Cifuentes
//  Fecha : 07/10/2026
//  Compilar: g++ -std=c++17 -o chapina main.cpp -lsqlite3
//  Ejecutar: ./chapina            (usa el archivo chapina.db)
//            ./chapina otra.db    (usa otro archivo de base de datos)
// ============================================================================
#include <sqlite3.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define ENTRADA_ES_TECLADO() _isatty(_fileno(stdin))
#else
#include <unistd.h>
#define ENTRADA_ES_TECLADO() isatty(STDIN_FILENO)
#endif

#include <climits>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

typedef vector<string> Fila;
typedef vector<Fila> Tabla;

static sqlite3* db = nullptr;

// ---------------------------------------------------------------- Sesión ---
enum Rol { ADMIN, VENDEDOR };
static Rol rolActual = ADMIN;
static string usuarioActual;
static string nombreUsuario;

static const double TASA_IVA = 0.12;  // IVA 12 %

// ============================================================================
//  1. UTILIDADES DE ENTRADA / SALIDA
// ============================================================================
static string recortar(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static string leer(const string& mensaje) {
    cout << mensaje;
    string s;
    if (!getline(cin, s)) {
        cout << "\n[Fin de la entrada. Cerrando el sistema]\n";
        if (db) sqlite3_close(db);
        exit(0);
    }
    s = recortar(s);
    // Si la entrada viene de un archivo (pruebas automáticas), se muestra lo "escrito"
    // para que la salida se lea igual que una sesión real en pantalla.
    if (!ENTRADA_ES_TECLADO()) cout << s << "\n";
    return s;
}

static string leerObligatorio(const string& mensaje) {
    while (true) {
        string s = leer(mensaje);
        if (!s.empty()) return s;
        cout << "  Este dato es obligatorio.\n";
    }
}

static string leerOpcional(const string& mensaje, const string& actual) {
    string s = leer(mensaje + " [" + actual + "]: ");
    return s.empty() ? actual : s;
}

static int leerEntero(const string& mensaje, int minimo, int maximo) {
    while (true) {
        string s = leer(mensaje);
        try {
            size_t pos = 0;
            long v = stol(s, &pos);
            if (pos == s.size() && v >= minimo && v <= maximo) return (int)v;
        } catch (...) {
        }
        cout << "  Valor inválido. Ingrese un número entero";
        if (maximo != INT_MAX) cout << " entre " << minimo << " y " << maximo;
        else cout << " mayor o igual a " << minimo;
        cout << ".\n";
    }
}

static double leerDecimal(const string& mensaje, double minimo) {
    while (true) {
        string s = leer(mensaje);
        try {
            size_t pos = 0;
            double v = stod(s, &pos);
            if (pos == s.size() && v >= minimo) return v;
        } catch (...) {
        }
        cout << "  Valor inválido. Ingrese un número mayor o igual a " << minimo << ".\n";
    }
}

static bool confirmar(const string& mensaje) {
    while (true) {
        string s = leer(mensaje + " (S/N): ");
        if (s == "S" || s == "s") return true;
        if (s == "N" || s == "n") return false;
        cout << "  Responda S o N.\n";
    }
}

static string num2(double v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

static double redondear(double v) { return round(v * 100.0) / 100.0; }

static string dinero(const string& s) {
    try {
        return num2(stod(s));
    } catch (...) {
        return s;
    }
}

static void titulo(const string& t) {
    cout << "\n--- " << t << " ---\n";
}

// Longitud de una cadena UTF-8 en caracteres (no en bytes), para alinear tablas.
static size_t largo(const string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) n++;
    return n;
}

static string ajustar(const string& texto, size_t ancho, bool derecha) {
    string t = texto;
    if (largo(t) > ancho) {
        size_t n = 0, i = 0;
        while (i < t.size() && n + 1 < ancho) {
            i++;
            while (i < t.size() && (((unsigned char)t[i] & 0xC0) == 0x80)) i++;
            n++;
        }
        t = t.substr(0, i) + "~";
    }
    size_t l = largo(t);
    string relleno(ancho > l ? ancho - l : 0, ' ');
    return derecha ? relleno + t : t + relleno;
}

// 'detalle' (opcional): texto que se imprime en una segunda línea bajo cada fila,
// útil para datos largos (direcciones, correos) que no caben en una columna.
static void imprimirTabla(const vector<string>& enc, const vector<int>& anch, const Tabla& filas,
                          const vector<bool>& der = {}, const vector<string>& detalle = {}) {
    int total = 0;
    for (int a : anch) total += a + 2;
    string sep(total, '-');
    cout << sep << "\n";
    for (size_t i = 0; i < enc.size(); i++)
        cout << ajustar(enc[i], anch[i], i < der.size() && der[i]) << "  ";
    cout << "\n" << sep << "\n";
    for (size_t k = 0; k < filas.size(); k++) {
        const Fila& f = filas[k];
        for (size_t i = 0; i < f.size() && i < anch.size(); i++)
            cout << ajustar(f[i], anch[i], i < der.size() && der[i]) << "  ";
        cout << "\n";
        if (k < detalle.size() && !detalle[k].empty()) cout << "      " << detalle[k] << "\n";
    }
    cout << sep << "\n";
    cout << "Total de registros: " << filas.size() << "\n";
}

// ============================================================================
//  2. ACCESO A BASE DE DATOS (SQLite)
// ============================================================================
// Traduce los errores técnicos de SQLite a mensajes comprensibles para el usuario.
static string traducirError(const string& m) {
    if (m.find("UNIQUE constraint failed: Empleado.codigo") != string::npos) return "Ya existe un empleado con ese código.";
    if (m.find("UNIQUE constraint failed: Empleado.dpi") != string::npos) return "Ya existe un empleado con ese DPI.";
    if (m.find("UNIQUE constraint failed: Cliente.nit") != string::npos) return "Ya existe un cliente con ese NIT.";
    if (m.find("UNIQUE constraint failed: Producto.codigo") != string::npos) return "Ya existe un producto con ese código.";
    if (m.find("FOREIGN KEY") != string::npos) return "El dato hace referencia a un registro que no existe.";
    if (m.find("CHECK constraint failed") != string::npos) return "Un valor está fuera del rango permitido.";
    return m;
}

// Ejecuta una sentencia con parámetros (?) y, opcionalmente, devuelve las filas.
static bool ejecutar(const string& sql, const Fila& params = {}, Tabla* salida = nullptr) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        cout << "  Error interno de base de datos: " << sqlite3_errmsg(db) << "\n";
        return false;
    }
    for (size_t i = 0; i < params.size(); i++)
        sqlite3_bind_text(st, (int)i + 1, params[i].c_str(), -1, SQLITE_TRANSIENT);

    int cols = sqlite3_column_count(st);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (salida) {
            Fila f;
            for (int c = 0; c < cols; c++) {
                const unsigned char* t = sqlite3_column_text(st, c);
                f.push_back(t ? reinterpret_cast<const char*>(t) : "");
            }
            salida->push_back(f);
        }
    }
    bool ok = (rc == SQLITE_DONE);
    if (!ok) cout << "  No se pudo completar la operación: " << traducirError(sqlite3_errmsg(db)) << "\n";
    sqlite3_finalize(st);
    return ok;
}

static void crearEsquema() {
    const char* esquema = R"SQL(
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS Empleado (
    codigo    TEXT PRIMARY KEY,
    dpi       TEXT NOT NULL UNIQUE,
    nombre    TEXT NOT NULL,
    telefono  TEXT,
    direccion TEXT,
    email     TEXT,
    tipo      TEXT NOT NULL CHECK (tipo IN ('Vendedor','Repartidor'))
);

CREATE TABLE IF NOT EXISTS Cliente (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    nombre          TEXT NOT NULL,
    nit             TEXT NOT NULL UNIQUE,
    telefono        TEXT,
    direccion       TEXT,
    contacto        TEXT,
    email           TEXT,
    web             TEXT,
    vendedor_codigo TEXT NOT NULL REFERENCES Empleado(codigo)
);

CREATE TABLE IF NOT EXISTS Producto (
    codigo     TEXT PRIMARY KEY,
    nombre     TEXT NOT NULL,
    costo      REAL NOT NULL CHECK (costo >= 0),
    precio     REAL NOT NULL CHECK (precio >= 0),
    existencia INTEGER NOT NULL CHECK (existencia >= 0)
);

CREATE TABLE IF NOT EXISTS Factura (
    correlativo       INTEGER PRIMARY KEY AUTOINCREMENT,
    serie             TEXT NOT NULL,
    fecha             TEXT NOT NULL,
    cliente_id        INTEGER NOT NULL REFERENCES Cliente(id),
    vendedor_codigo   TEXT NOT NULL REFERENCES Empleado(codigo),
    condicion         TEXT NOT NULL CHECK (condicion IN ('Contado','Crédito')),
    plazo             INTEGER NOT NULL DEFAULT 0,
    fecha_vencimiento TEXT NOT NULL,
    subtotal          REAL NOT NULL,
    iva               REAL NOT NULL,
    total             REAL NOT NULL
);

CREATE TABLE IF NOT EXISTS DetalleFactura (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    factura_correlativo INTEGER NOT NULL REFERENCES Factura(correlativo),
    producto_codigo     TEXT NOT NULL REFERENCES Producto(codigo),
    cantidad            INTEGER NOT NULL CHECK (cantidad > 0),
    precio_unitario     REAL NOT NULL,
    subtotal            REAL NOT NULL
);

CREATE TABLE IF NOT EXISTS Pago (
    correlativo         INTEGER PRIMARY KEY AUTOINCREMENT,
    factura_correlativo INTEGER NOT NULL REFERENCES Factura(correlativo),
    fecha               TEXT NOT NULL,
    monto               REAL NOT NULL CHECK (monto > 0),
    saldo               REAL NOT NULL CHECK (saldo >= 0)
);
)SQL";
    char* err = nullptr;
    if (sqlite3_exec(db, esquema, nullptr, nullptr, &err) != SQLITE_OK) {
        cerr << "No se pudo crear la base de datos: " << (err ? err : "") << "\n";
        sqlite3_free(err);
        sqlite3_close(db);
        exit(1);
    }
}

static string saldoFactura(const string& correlativo) {
    Tabla t;
    ejecutar("SELECT ROUND(total - COALESCE((SELECT SUM(monto) FROM Pago WHERE factura_correlativo = f.correlativo), 0), 2) "
             "FROM Factura f WHERE correlativo = ?",
             {correlativo}, &t);
    return t.empty() ? "" : t[0][0];
}

// ============================================================================
//  3. MÓDULO DE EMPLEADOS  (Vendedores y Repartidores)
// ============================================================================
static void listarEmpleados() {
    Tabla t;
    ejecutar("SELECT codigo, dpi, nombre, telefono, email, tipo FROM Empleado ORDER BY codigo", {}, &t);
    imprimirTabla({"Código", "DPI", "Nombre", "Teléfono", "E-mail", "Tipo"}, {8, 14, 24, 10, 24, 10}, t);
}

static void registrarEmpleado() {
    titulo("Registrar empleado");
    string codigo = leerObligatorio("Código de empleado : ");
    string dpi = leerObligatorio("DPI                : ");
    string nombre = leerObligatorio("Nombre             : ");
    string tel = leer("Teléfono           : ");
    string dir = leer("Dirección          : ");
    string mail = leer("E-mail             : ");
    int tipo = leerEntero("Tipo (1=Vendedor, 2=Repartidor): ", 1, 2);
    if (ejecutar("INSERT INTO Empleado(codigo,dpi,nombre,telefono,direccion,email,tipo) VALUES(?,?,?,?,?,?,?)",
                 {codigo, dpi, nombre, tel, dir, mail, tipo == 1 ? "Vendedor" : "Repartidor"}))
        cout << "Empleado registrado correctamente.\n";
}

static void modificarEmpleado() {
    titulo("Modificar empleado");
    listarEmpleados();
    string codigo = leerObligatorio("Código del empleado a modificar: ");
    Tabla t;
    ejecutar("SELECT nombre, telefono, direccion, email FROM Empleado WHERE codigo = ?", {codigo}, &t);
    if (t.empty()) {
        cout << "No existe un empleado con ese código.\n";
        return;
    }
    cout << "(Presione Enter para conservar el valor actual)\n";
    string nombre = leerOpcional("Nombre   ", t[0][0]);
    string tel = leerOpcional("Teléfono ", t[0][1]);
    string dir = leerOpcional("Dirección", t[0][2]);
    string mail = leerOpcional("E-mail   ", t[0][3]);
    if (ejecutar("UPDATE Empleado SET nombre=?, telefono=?, direccion=?, email=? WHERE codigo=?",
                 {nombre, tel, dir, mail, codigo}))
        cout << "Empleado actualizado correctamente.\n";
}

// ============================================================================
//  4. MÓDULO DE CLIENTES
// ============================================================================
static void listarClientes(bool soloPropios) {
    Tabla t;
    string sql = "SELECT c.id, c.nombre, c.nit, c.telefono, c.contacto, c.vendedor_codigo FROM Cliente c";
    Fila p;
    if (soloPropios) {
        sql += " WHERE c.vendedor_codigo = ?";
        p.push_back(usuarioActual);
    }
    sql += " ORDER BY c.id";
    ejecutar(sql, p, &t);
    imprimirTabla({"ID", "Nombre", "NIT", "Teléfono", "Contacto", "Vendedor"}, {4, 24, 11, 10, 18, 8}, t,
                  {true, false, false, false, false, false});
}

static void registrarCliente() {
    titulo("Registrar cliente");
    string nombre = leerObligatorio("Nombre           : ");
    string nit = leerObligatorio("NIT              : ");
    string tel = leer("Teléfono         : ");
    string dir = leer("Dirección        : ");
    string contacto = leer("Persona contacto : ");
    string mail = leer("E-mail           : ");
    string web = leer("Página web       : ");

    string vendedor;
    if (rolActual == VENDEDOR) {
        vendedor = usuarioActual;
        cout << "Vendedor asignado: " << vendedor << " (usted)\n";
    } else {
        Tabla v;
        ejecutar("SELECT codigo, nombre FROM Empleado WHERE tipo='Vendedor' ORDER BY codigo", {}, &v);
        imprimirTabla({"Código", "Vendedor"}, {8, 30}, v);
        vendedor = leerObligatorio("Código del vendedor asignado: ");
        Tabla ok;
        ejecutar("SELECT codigo FROM Empleado WHERE codigo=? AND tipo='Vendedor'", {vendedor}, &ok);
        if (ok.empty()) {
            cout << "Ese código no corresponde a un Vendedor registrado.\n";
            return;
        }
    }
    if (ejecutar("INSERT INTO Cliente(nombre,nit,telefono,direccion,contacto,email,web,vendedor_codigo) VALUES(?,?,?,?,?,?,?,?)",
                 {nombre, nit, tel, dir, contacto, mail, web, vendedor}))
        cout << "Cliente registrado correctamente.\n";
}

static void modificarCliente() {
    titulo("Modificar cliente");
    listarClientes(rolActual == VENDEDOR);
    string id = leerObligatorio("ID del cliente a modificar: ");
    Tabla t;
    string sql = "SELECT nombre, telefono, direccion, contacto, email, web FROM Cliente WHERE id = ?";
    Fila p = {id};
    if (rolActual == VENDEDOR) {
        sql += " AND vendedor_codigo = ?";
        p.push_back(usuarioActual);
    }
    ejecutar(sql, p, &t);
    if (t.empty()) {
        cout << "No existe un cliente con ese ID en su cartera.\n";
        return;
    }
    cout << "(Presione Enter para conservar el valor actual)\n";
    string nombre = leerOpcional("Nombre          ", t[0][0]);
    string tel = leerOpcional("Teléfono        ", t[0][1]);
    string dir = leerOpcional("Dirección       ", t[0][2]);
    string con = leerOpcional("Persona contacto", t[0][3]);
    string mail = leerOpcional("E-mail          ", t[0][4]);
    string web = leerOpcional("Página web      ", t[0][5]);
    if (ejecutar("UPDATE Cliente SET nombre=?, telefono=?, direccion=?, contacto=?, email=?, web=? WHERE id=?",
                 {nombre, tel, dir, con, mail, web, id}))
        cout << "Cliente actualizado correctamente.\n";
}

static void consultarClientes() {
    titulo("Clientes registrados");
    listarClientes(rolActual == VENDEDOR);
}

static void historialCliente() {
    titulo("Historial de un cliente");
    listarClientes(rolActual == VENDEDOR);
    string id = leerObligatorio("ID del cliente: ");
    Tabla c;
    string sql = "SELECT nombre, nit FROM Cliente WHERE id = ?";
    Fila p = {id};
    if (rolActual == VENDEDOR) {
        sql += " AND vendedor_codigo = ?";
        p.push_back(usuarioActual);
    }
    ejecutar(sql, p, &c);
    if (c.empty()) {
        cout << "No existe un cliente con ese ID en su cartera.\n";
        return;
    }
    cout << "\nCliente: " << c[0][0] << "   NIT: " << c[0][1] << "\n\nFacturas:\n";
    Tabla f;
    ejecutar("SELECT f.correlativo, f.fecha, f.condicion, f.total, "
             "ROUND(f.total - COALESCE((SELECT SUM(monto) FROM Pago WHERE factura_correlativo=f.correlativo),0),2) "
             "FROM Factura f WHERE f.cliente_id = ? ORDER BY f.correlativo",
             {id}, &f);
    for (Fila& r : f) {
        r[3] = dinero(r[3]);
        r[4] = dinero(r[4]);
    }
    imprimirTabla({"Factura", "Fecha", "Condición", "Total", "Saldo"}, {8, 10, 10, 12, 12}, f,
                  {true, false, false, true, true});

    cout << "\nPagos realizados:\n";
    Tabla pg;
    ejecutar("SELECT p.correlativo, p.factura_correlativo, p.fecha, p.monto, p.saldo FROM Pago p "
             "JOIN Factura f ON f.correlativo = p.factura_correlativo WHERE f.cliente_id = ? ORDER BY p.correlativo",
             {id}, &pg);
    for (Fila& r : pg) {
        r[3] = dinero(r[3]);
        r[4] = dinero(r[4]);
    }
    imprimirTabla({"Pago", "Factura", "Fecha", "Monto", "Saldo"}, {6, 8, 10, 12, 12}, pg,
                  {true, true, false, true, true});
}

// ============================================================================
//  5. MÓDULO DE PRODUCTOS E INVENTARIO
// ============================================================================
static void listarProductos() {
    Tabla t;
    ejecutar("SELECT codigo, nombre, costo, precio, existencia FROM Producto ORDER BY codigo", {}, &t);
    for (Fila& r : t) {
        r[2] = dinero(r[2]);
        r[3] = dinero(r[3]);
    }
    imprimirTabla({"Código", "Producto", "Costo", "Precio", "Existencia"}, {8, 28, 10, 10, 10}, t,
                  {false, false, true, true, true});
}

static void registrarProducto() {
    titulo("Registrar producto");
    string codigo = leerObligatorio("Código     : ");
    string nombre = leerObligatorio("Nombre     : ");
    double costo = leerDecimal("Costo (Q)  : ", 0);
    double precio = leerDecimal("Precio (Q) : ", 0);
    if (precio < costo) cout << "  Aviso: el precio de venta es menor al costo.\n";
    int exist = leerEntero("Existencia : ", 0, INT_MAX);
    if (ejecutar("INSERT INTO Producto(codigo,nombre,costo,precio,existencia) VALUES(?,?,?,?,?)",
                 {codigo, nombre, num2(costo), num2(precio), to_string(exist)}))
        cout << "Producto registrado correctamente.\n";
}

static void modificarProducto() {
    titulo("Modificar producto");
    listarProductos();
    string codigo = leerObligatorio("Código del producto: ");
    Tabla t;
    ejecutar("SELECT nombre, costo, precio FROM Producto WHERE codigo = ?", {codigo}, &t);
    if (t.empty()) {
        cout << "No existe un producto con ese código.\n";
        return;
    }
    cout << "(Presione Enter para conservar el valor actual)\n";
    string nombre = leerOpcional("Nombre      ", t[0][0]);
    double costo = stod(t[0][1]), precio = stod(t[0][2]);
    string c = leer("Costo (Q) [" + dinero(t[0][1]) + "]: ");
    string p = leer("Precio (Q) [" + dinero(t[0][2]) + "]: ");
    try {
        if (!c.empty()) costo = stod(c);
        if (!p.empty()) precio = stod(p);
    } catch (...) {
        cout << "Valor numérico inválido. No se realizaron cambios.\n";
        return;
    }
    if (costo < 0 || precio < 0) {
        cout << "Los valores no pueden ser negativos. No se realizaron cambios.\n";
        return;
    }
    if (ejecutar("UPDATE Producto SET nombre=?, costo=?, precio=? WHERE codigo=?",
                 {nombre, num2(costo), num2(precio), codigo}))
        cout << "Producto actualizado correctamente.\n";
}

static void reabastecerProducto() {
    titulo("Ingreso de mercadería (inventario)");
    listarProductos();
    string codigo = leerObligatorio("Código del producto: ");
    Tabla t;
    ejecutar("SELECT nombre, existencia FROM Producto WHERE codigo = ?", {codigo}, &t);
    if (t.empty()) {
        cout << "No existe un producto con ese código.\n";
        return;
    }
    int cant = leerEntero("Cantidad que ingresa a bodega: ", 1, INT_MAX);
    if (ejecutar("UPDATE Producto SET existencia = existencia + ? WHERE codigo = ?", {to_string(cant), codigo}))
        cout << "Existencia actualizada: " << t[0][0] << " ahora tiene " << (stoi(t[0][1]) + cant) << " unidades.\n";
}

// ============================================================================
//  6. MÓDULO DE FACTURACIÓN
// ============================================================================
static void mostrarFactura(const string& correlativo) {
    Tabla f;
    ejecutar("SELECT f.correlativo, f.serie, f.fecha, c.nombre, c.nit, e.nombre, f.condicion, f.plazo, "
             "f.fecha_vencimiento, f.subtotal, f.iva, f.total "
             "FROM Factura f JOIN Cliente c ON c.id = f.cliente_id JOIN Empleado e ON e.codigo = f.vendedor_codigo "
             "WHERE f.correlativo = ?",
             {correlativo}, &f);
    if (f.empty()) {
        cout << "No existe la factura " << correlativo << ".\n";
        return;
    }
    const Fila& r = f[0];
    cout << "\n==================== FACTURA ====================\n";
    cout << "No. correlativo : " << r[0] << "     Serie: " << r[1] << "\n";
    cout << "Fecha           : " << r[2] << "\n";
    cout << "Cliente         : " << r[3] << "   (NIT " << r[4] << ")\n";
    cout << "Vendedor        : " << r[5] << "\n";
    cout << "Condición       : " << r[6];
    if (r[6] == "Crédito") cout << "  (plazo " << r[7] << " días, vence " << r[8] << ")";
    cout << "\n";
    Tabla d;
    ejecutar("SELECT d.producto_codigo, p.nombre, d.cantidad, d.precio_unitario, d.subtotal "
             "FROM DetalleFactura d JOIN Producto p ON p.codigo = d.producto_codigo "
             "WHERE d.factura_correlativo = ? ORDER BY d.id",
             {correlativo}, &d);
    for (Fila& x : d) {
        x[3] = dinero(x[3]);
        x[4] = dinero(x[4]);
    }
    imprimirTabla({"Código", "Producto", "Cant.", "P. unit.", "Subtotal"}, {8, 26, 6, 10, 12}, d,
                  {false, false, true, true, true});
    cout << "Subtotal : Q " << dinero(r[9]) << "\n";
    cout << "IVA 12%  : Q " << dinero(r[10]) << "\n";
    cout << "TOTAL    : Q " << dinero(r[11]) << "\n";
    cout << "Saldo    : Q " << dinero(saldoFactura(correlativo)) << "\n";
    cout << "=================================================\n";
}

struct Linea {
    string codigo, nombre;
    int cantidad;
    double precio;
};

static void generarFactura() {
    titulo("Generar factura");
    listarClientes(rolActual == VENDEDOR);
    int cid = leerEntero("ID del cliente: ", 1, INT_MAX);
    Tabla c;
    ejecutar("SELECT id, nombre, vendedor_codigo FROM Cliente WHERE id = ?", {to_string(cid)}, &c);
    if (c.empty()) {
        cout << "Cliente no encontrado.\n";
        return;
    }
    if (rolActual == VENDEDOR && c[0][2] != usuarioActual) {
        cout << "Ese cliente no pertenece a su cartera. No puede facturarle.\n";
        return;
    }
    cout << "Cliente: " << c[0][1] << "  |  Vendedor asignado: " << c[0][2] << "\n";

    int cond = leerEntero("Condición (1=Contado, 2=Crédito): ", 1, 2);
    int plazo = 0;
    if (cond == 2) plazo = leerEntero("Plazo en días (1-365): ", 1, 365);

    cout << "\nProductos disponibles:\n";
    listarProductos();

    vector<Linea> lineas;
    while (true) {
        string cod = leer("Código de producto (Enter para terminar): ");
        if (cod.empty()) break;
        Tabla p;
        ejecutar("SELECT codigo, nombre, precio, existencia FROM Producto WHERE codigo = ?", {cod}, &p);
        if (p.empty()) {
            cout << "  Producto no encontrado.\n";
            continue;
        }
        int yaPedido = 0;
        for (const Linea& l : lineas)
            if (l.codigo == p[0][0]) yaPedido += l.cantidad;
        int disponible = stoi(p[0][3]) - yaPedido;
        if (disponible <= 0) {
            cout << "  Sin existencia disponible de " << p[0][1] << ".\n";
            continue;
        }
        int cant = leerEntero("Cantidad (disponible " + to_string(disponible) + "): ", 1, INT_MAX);
        if (cant > disponible) {
            cout << "  Existencia insuficiente. Solo hay " << disponible << " unidades disponibles.\n";
            continue;
        }
        lineas.push_back({p[0][0], p[0][1], cant, stod(p[0][2])});
        cout << "  Agregado: " << cant << " x " << p[0][1] << " a Q " << dinero(p[0][2]) << " c/u\n";
    }
    if (lineas.empty()) {
        cout << "No se agregó ningún producto. Factura cancelada.\n";
        return;
    }

    double subtotal = 0;
    for (const Linea& l : lineas) subtotal += redondear(l.cantidad * l.precio);
    subtotal = redondear(subtotal);
    double iva = redondear(subtotal * TASA_IVA);
    double total = redondear(subtotal + iva);

    cout << "\nResumen: Subtotal Q " << num2(subtotal) << " | IVA Q " << num2(iva) << " | TOTAL Q " << num2(total) << "\n";
    if (!confirmar("¿Confirmar la factura?")) {
        cout << "Factura cancelada.\n";
        return;
    }

    // Todo se guarda en una transacción: o se guarda completo o no se guarda nada.
    ejecutar("BEGIN");
    bool ok = ejecutar("INSERT INTO Factura(serie,fecha,cliente_id,vendedor_codigo,condicion,plazo,fecha_vencimiento,subtotal,iva,total) "
                       "VALUES('A', date('now','localtime'), ?, ?, ?, ?, date('now','localtime','+' || ? || ' days'), ?, ?, ?)",
                       {to_string(cid), c[0][2], cond == 1 ? "Contado" : "Crédito", to_string(plazo), to_string(plazo),
                        num2(subtotal), num2(iva), num2(total)});
    string correlativo = to_string((long long)sqlite3_last_insert_rowid(db));

    for (size_t i = 0; ok && i < lineas.size(); i++) {
        const Linea& l = lineas[i];
        ok = ejecutar("INSERT INTO DetalleFactura(factura_correlativo,producto_codigo,cantidad,precio_unitario,subtotal) VALUES(?,?,?,?,?)",
                      {correlativo, l.codigo, to_string(l.cantidad), num2(l.precio), num2(redondear(l.cantidad * l.precio))});
        if (ok) {
            ok = ejecutar("UPDATE Producto SET existencia = existencia - ? WHERE codigo = ? AND existencia >= ?",
                          {to_string(l.cantidad), l.codigo, to_string(l.cantidad)});
            if (ok && sqlite3_changes(db) != 1) ok = false;
        }
    }
    if (ok && cond == 1)  // Contado: un solo pago por el total
        ok = ejecutar("INSERT INTO Pago(factura_correlativo,fecha,monto,saldo) VALUES(?, date('now','localtime'), ?, 0)",
                      {correlativo, num2(total)});

    if (ok) {
        ejecutar("COMMIT");
        cout << "\nFactura generada correctamente.\n";
        mostrarFactura(correlativo);
    } else {
        ejecutar("ROLLBACK");
        cout << "No se pudo generar la factura. No se realizó ningún cambio.\n";
    }
}

static void consultarFactura() {
    titulo("Consultar factura");
    Tabla t;
    string sql = "SELECT f.correlativo, f.fecha, c.nombre, f.condicion, f.total FROM Factura f "
                 "JOIN Cliente c ON c.id = f.cliente_id";
    Fila p;
    if (rolActual == VENDEDOR) {
        sql += " WHERE f.vendedor_codigo = ?";
        p.push_back(usuarioActual);
    }
    sql += " ORDER BY f.correlativo";
    ejecutar(sql, p, &t);
    for (Fila& r : t) r[4] = dinero(r[4]);
    imprimirTabla({"Factura", "Fecha", "Cliente", "Condición", "Total"}, {8, 10, 24, 10, 12}, t,
                  {true, false, false, false, true});
    if (t.empty()) return;
    int n = leerEntero("Número de factura a ver (0 para volver): ", 0, INT_MAX);
    if (n == 0) return;
    // un vendedor solo puede ver sus propias facturas
    bool permitido = false;
    for (const Fila& r : t)
        if (r[0] == to_string(n)) permitido = true;
    if (!permitido) {
        cout << "Factura no encontrada.\n";
        return;
    }
    mostrarFactura(to_string(n));
}

// ============================================================================
//  7. MÓDULO DE PAGOS
// ============================================================================
static void registrarPago() {
    titulo("Registrar pago");
    Tabla t;
    string sql = "SELECT f.correlativo, f.fecha, c.nombre, f.condicion, f.total, "
                 "ROUND(f.total - COALESCE((SELECT SUM(monto) FROM Pago WHERE factura_correlativo=f.correlativo),0),2) AS saldo "
                 "FROM Factura f JOIN Cliente c ON c.id = f.cliente_id WHERE saldo > 0";
    Fila p;
    if (rolActual == VENDEDOR) {
        sql += " AND f.vendedor_codigo = ?";
        p.push_back(usuarioActual);
    }
    sql += " ORDER BY f.correlativo";
    ejecutar(sql, p, &t);
    if (t.empty()) {
        cout << "No hay facturas con saldo pendiente.\n";
        return;
    }
    for (Fila& r : t) {
        r[4] = dinero(r[4]);
        r[5] = dinero(r[5]);
    }
    cout << "Facturas con saldo pendiente:\n";
    imprimirTabla({"Factura", "Fecha", "Cliente", "Condición", "Total", "Saldo"}, {8, 10, 22, 10, 12, 12}, t,
                  {true, false, false, false, true, true});

    string corr = leerObligatorio("Número de factura a abonar: ");
    string saldoTxt;
    for (const Fila& r : t)
        if (r[0] == corr) saldoTxt = r[5];
    if (saldoTxt.empty()) {
        cout << "Esa factura no tiene saldo pendiente o no está disponible para usted.\n";
        return;
    }
    double saldo = stod(saldoTxt);
    cout << "Saldo pendiente: Q " << num2(saldo) << "\n";
    double monto = leerDecimal("Monto del pago (Q): ", 0.01);
    monto = redondear(monto);
    if (monto > saldo + 0.001) {
        cout << "El monto (Q " << num2(monto) << ") excede el saldo pendiente. Pago no registrado.\n";
        return;
    }
    double nuevo = redondear(saldo - monto);
    if (ejecutar("INSERT INTO Pago(factura_correlativo,fecha,monto,saldo) VALUES(?, date('now','localtime'), ?, ?)",
                 {corr, num2(monto), num2(nuevo)})) {
        cout << "Pago registrado. Saldo restante de la factura " << corr << ": Q " << num2(nuevo) << "\n";
        if (nuevo == 0) cout << "La factura quedó cancelada en su totalidad.\n";
    }
}

static void consultarPagos() {
    titulo("Pagos de una factura");
    int n = leerEntero("Número de factura: ", 1, INT_MAX);
    Tabla f;
    string sql = "SELECT correlativo FROM Factura WHERE correlativo = ?";
    Fila p = {to_string(n)};
    if (rolActual == VENDEDOR) {
        sql += " AND vendedor_codigo = ?";
        p.push_back(usuarioActual);
    }
    ejecutar(sql, p, &f);
    if (f.empty()) {
        cout << "Factura no encontrada.\n";
        return;
    }
    Tabla t;
    ejecutar("SELECT correlativo, fecha, monto, saldo FROM Pago WHERE factura_correlativo = ? ORDER BY correlativo",
             {to_string(n)}, &t);
    for (Fila& r : t) {
        r[2] = dinero(r[2]);
        r[3] = dinero(r[3]);
    }
    imprimirTabla({"Pago", "Fecha", "Monto", "Saldo"}, {6, 10, 12, 12}, t, {true, false, true, true});
    cout << "Saldo actual de la factura: Q " << dinero(saldoFactura(to_string(n))) << "\n";
}

// ============================================================================
//  8. MÓDULO DE REPORTES
// ============================================================================
static void reporteClientes() {
    titulo("REPORTE DE CLIENTES");
    Tabla t;
    ejecutar("SELECT id, nombre, nit, telefono, contacto, vendedor_codigo, direccion, email, web FROM Cliente ORDER BY id", {}, &t);
    Tabla principal;
    vector<string> detalle;
    for (const Fila& r : t) {
        principal.push_back({r[0], r[1], r[2], r[3], r[4], r[5]});
        string d = "Dirección: " + r[6];
        if (!r[7].empty()) d += "  |  E-mail: " + r[7];
        if (!r[8].empty()) d += "  |  Web: " + r[8];
        detalle.push_back(d);
    }
    imprimirTabla({"ID", "Nombre", "NIT", "Teléfono", "Contacto", "Vendedor"}, {4, 24, 11, 10, 18, 8}, principal,
                  {true}, detalle);
}

static void reporteVendedores() {
    titulo("REPORTE DE VENDEDORES");
    Tabla t;
    ejecutar("SELECT e.codigo, e.dpi, e.nombre, e.telefono, e.email, "
             "(SELECT COUNT(*) FROM Cliente c WHERE c.vendedor_codigo = e.codigo), e.direccion "
             "FROM Empleado e WHERE e.tipo = 'Vendedor' ORDER BY e.codigo",
             {}, &t);
    Tabla principal;
    vector<string> detalle;
    for (const Fila& r : t) {
        principal.push_back({r[0], r[1], r[2], r[3], r[4], r[5]});
        detalle.push_back("Dirección: " + r[6]);
    }
    imprimirTabla({"Código", "DPI", "Nombre", "Teléfono", "E-mail", "Clientes"}, {7, 13, 22, 9, 22, 8}, principal,
                  {false, false, false, false, false, true}, detalle);
}

static void reporteProductosVendidos() {
    titulo("REPORTE DE PRODUCTOS VENDIDOS");
    string desde = leer("Desde (AAAA-MM-DD, Enter = sin límite): ");
    string hasta = leer("Hasta (AAAA-MM-DD, Enter = sin límite): ");
    Tabla t;
    ejecutar("SELECT d.producto_codigo, p.nombre, SUM(d.cantidad), SUM(d.subtotal) "
             "FROM DetalleFactura d JOIN Producto p ON p.codigo = d.producto_codigo "
             "JOIN Factura f ON f.correlativo = d.factura_correlativo "
             "WHERE (? = '' OR f.fecha >= ?) AND (? = '' OR f.fecha <= ?) "
             "GROUP BY d.producto_codigo, p.nombre ORDER BY SUM(d.cantidad) DESC",
             {desde, desde, hasta, hasta}, &t);
    for (Fila& r : t) r[3] = dinero(r[3]);
    imprimirTabla({"Código", "Producto", "Unidades", "Monto vendido"}, {8, 28, 9, 14}, t, {false, false, true, true});
}

static void reporteVentasDelDia() {
    titulo("LISTADO DE VENTAS (FACTURAS DEL DÍA)");
    Tabla t;
    ejecutar("SELECT f.correlativo, f.serie, c.nombre, e.nombre, f.condicion, f.subtotal, f.iva, f.total "
             "FROM Factura f JOIN Cliente c ON c.id = f.cliente_id JOIN Empleado e ON e.codigo = f.vendedor_codigo "
             "WHERE f.fecha = date('now','localtime') ORDER BY f.correlativo",
             {}, &t);
    double suma = 0;
    for (Fila& r : t) {
        suma += stod(r[7]);
        r[5] = dinero(r[5]);
        r[6] = dinero(r[6]);
        r[7] = dinero(r[7]);
    }
    imprimirTabla({"Factura", "Serie", "Cliente", "Vendedor", "Condición", "Subtotal", "IVA", "Total"},
                  {8, 5, 22, 22, 10, 10, 9, 11}, t, {true, false, false, false, false, true, true, true});
    cout << "TOTAL VENDIDO DEL DÍA: Q " << num2(suma) << "\n";
}

static void reportePagosDelDia() {
    titulo("REPORTE DE PAGOS DEL DÍA");
    Tabla t;
    ejecutar("SELECT p.correlativo, p.factura_correlativo, c.nombre, f.condicion, p.monto, p.saldo "
             "FROM Pago p JOIN Factura f ON f.correlativo = p.factura_correlativo JOIN Cliente c ON c.id = f.cliente_id "
             "WHERE p.fecha = date('now','localtime') ORDER BY p.correlativo",
             {}, &t);
    double suma = 0;
    for (Fila& r : t) {
        suma += stod(r[4]);
        r[4] = dinero(r[4]);
        r[5] = dinero(r[5]);
    }
    imprimirTabla({"Pago", "Factura", "Cliente", "Condición", "Monto", "Saldo"}, {6, 8, 24, 10, 12, 12}, t,
                  {true, true, false, false, true, true});
    cout << "TOTAL RECIBIDO DEL DÍA: Q " << num2(suma) << "\n";
}

static void reporteInventario() {
    titulo("REPORTE DE INVENTARIO EXISTENTE");
    Tabla t;
    ejecutar("SELECT codigo, nombre, costo, precio, existencia FROM Producto ORDER BY codigo", {}, &t);
    for (Fila& r : t) {
        r[2] = dinero(r[2]);
        r[3] = dinero(r[3]);
    }
    imprimirTabla({"Código", "Producto", "Costo", "Precio", "Existencia"}, {8, 28, 10, 10, 10}, t,
                  {false, false, true, true, true});
}

// ============================================================================
//  9. MENÚS Y SESIÓN
// ============================================================================
struct Opcion {
    string texto;
    void (*accion)();
};

static void correrMenu(const string& nombre, const vector<Opcion>& ops, const string& textoSalida) {
    while (true) {
        cout << "\n=========== " << nombre << " ===========\n";
        for (size_t i = 0; i < ops.size(); i++) cout << "  " << (i + 1) << ". " << ops[i].texto << "\n";
        cout << "  0. " << textoSalida << "\n";
        int o = leerEntero("Seleccione una opción: ", 0, (int)ops.size());
        if (o == 0) return;
        ops[o - 1].accion();
    }
}

static void menuEmpleados() {
    correrMenu("EMPLEADOS", {{"Registrar empleado", registrarEmpleado},
                             {"Modificar empleado", modificarEmpleado},
                             {"Consultar empleados", []() { titulo("Empleados registrados"); listarEmpleados(); }}},
               "Volver");
}
static void menuClientes() {
    correrMenu("CLIENTES", {{"Registrar cliente", registrarCliente},
                            {"Modificar cliente", modificarCliente},
                            {"Consultar clientes", consultarClientes},
                            {"Historial de un cliente (facturas y pagos)", historialCliente}},
               "Volver");
}
static void menuProductos() {
    correrMenu("PRODUCTOS E INVENTARIO", {{"Registrar producto", registrarProducto},
                                          {"Modificar producto", modificarProducto},
                                          {"Ingreso de mercadería (aumentar existencia)", reabastecerProducto},
                                          {"Consultar productos", []() { titulo("Productos"); listarProductos(); }}},
               "Volver");
}
static void menuFacturacion() {
    correrMenu("FACTURACIÓN", {{"Generar factura", generarFactura}, {"Consultar factura", consultarFactura}}, "Volver");
}
static void menuPagos() {
    correrMenu("PAGOS", {{"Registrar pago", registrarPago}, {"Consultar pagos de una factura", consultarPagos}}, "Volver");
}
static void menuReportes() {
    correrMenu("REPORTES", {{"Reporte de Clientes", reporteClientes},
                            {"Reporte de Vendedores", reporteVendedores},
                            {"Reporte de Productos Vendidos", reporteProductosVendidos},
                            {"Listado de Ventas (Facturas del Día)", reporteVentasDelDia},
                            {"Reporte de Pagos del Día", reportePagosDelDia},
                            {"Reporte de Inventario Existente", reporteInventario}},
               "Volver");
}

static bool iniciarSesion() {
    cout << "======================================================\n";
    cout << "  SISTEMA DE FACTURACIÓN - REPRESENTACIONES \"CHAPINA\"\n";
    cout << "======================================================\n";
    for (int intento = 1; intento <= 3; intento++) {
        string u = leer("Usuario (admin o código de empleado): ");
        string c = leer("Clave: ");
        if (u == "admin" && c == "admin123") {
            rolActual = ADMIN;
            usuarioActual = "admin";
            nombreUsuario = "Administrador";
            return true;
        }
        Tabla t;
        ejecutar("SELECT nombre FROM Empleado WHERE codigo = ? AND dpi = ? AND tipo = 'Vendedor'", {u, c}, &t);
        if (!t.empty()) {
            rolActual = VENDEDOR;
            usuarioActual = u;
            nombreUsuario = t[0][0];
            return true;
        }
        cout << "Credenciales incorrectas (intento " << intento << " de 3).\n";
    }
    return false;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);  // para mostrar tildes y ñ correctamente en la consola de Windows
    SetConsoleCP(CP_UTF8);
#endif
    string ruta = (argc > 1) ? argv[1] : "chapina.db";
    if (sqlite3_open(ruta.c_str(), &db) != SQLITE_OK) {
        cerr << "No se pudo abrir la base de datos: " << sqlite3_errmsg(db) << "\n";
        return 1;
    }
    crearEsquema();

    if (!iniciarSesion()) {
        cout << "Demasiados intentos fallidos. El sistema se cerrará.\n";
        sqlite3_close(db);
        return 1;
    }
    if (rolActual == ADMIN) cout << "\nBienvenido(a), Administrador\n";
    else cout << "\nBienvenido(a), " << nombreUsuario << " (Vendedor)\n";

    if (rolActual == ADMIN) {
        correrMenu("MENÚ PRINCIPAL", {{"Empleados", menuEmpleados},
                                      {"Clientes", menuClientes},
                                      {"Productos e Inventario", menuProductos},
                                      {"Facturación", menuFacturacion},
                                      {"Pagos", menuPagos},
                                      {"Reportes", menuReportes}},
                   "Salir del sistema");
    } else {
        correrMenu("MENÚ PRINCIPAL", {{"Clientes", menuClientes}, {"Facturación", menuFacturacion}, {"Pagos", menuPagos}},
                   "Salir del sistema");
    }
    cout << "\nSesión finalizada. ¡Hasta luego!\n";
    sqlite3_close(db);
    return 0;
}
