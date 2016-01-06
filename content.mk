# build dir location, do not change the var name
build_dir = build

# install dir location, do not change the var name
install_dir = .

# Do not change the following target var name
# If you need to customize these names, remeber to change those
# ones in build.py at the same time.
# Search for build_defs in build.py, that's the only place
# these names are used.
ipc_bin = client server

# Define include dir
# Do not change the include dir name
INCLUDE_DIR = -I .

# Define CFLAGS
# Do not change the include dir name
MK_CFLAGS = -Wall -Werror

client = client.c \
         generic_ipc.c \
         util.c

client_LDSO = -lrt -lpthread

server = server.c \
         generic_ipc.c \
         util.c

server_LDSO = -lrt -lpthread
