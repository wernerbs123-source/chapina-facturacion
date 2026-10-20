# Uso:  make        (compila)      make run     (compila y ejecuta con la BD de demostración)
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra

chapina: src/main.cpp
	$(CXX) $(CXXFLAGS) -o chapina src/main.cpp -lsqlite3

run: chapina
	./chapina db/chapina_demo.db

clean:
	rm -f chapina chapina.exe
