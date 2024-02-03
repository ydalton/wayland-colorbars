CFLAGS	:=	-O2 -g -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration -Werror=incompatible-pointer-types
LDFLAGS	:=	-lwayland-client
OBJ	:=	wayland.o xdg-shell-protocol.o xdg-decoration-unstable-v1.o
HEADER	:=	xdg-shell-client-protocol.h
PRIV	:=	xdg-shell-protocol.c
XML	:=	/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
HEADER2	:=	xdg-decoration-unstable-v1.h
PRIV2	:=	xdg-decoration-unstable-v1.c
XML2	:=	/usr/share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
TARGET	:=	app

$(TARGET): $(OBJ)
	$(CC) -o $(TARGET) $(OBJ) $(LDFLAGS)

%.o: %.c $(HEADER) $(HEADER2)
	$(CC) -c -o $@ $< $(CFLAGS)

$(HEADER): 
	wayland-scanner client-header < $(XML) > $(HEADER)
	wayland-scanner private-code < $(XML) > $(PRIV)

$(HEADER2):
	wayland-scanner private-code < $(XML2) > $(PRIV2)
	wayland-scanner client-header < $(XML2) > $(HEADER2)


clean:
	rm -f $(OBJ) $(TARGET) $(PRIV) $(PRIV2) $(HEADER) $(HEADER2)

.PHONY: clean $(TARGET)
