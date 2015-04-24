CFLAGS=-Werror -Wno-missing-field-initializers -Wextra -Wall -O2 -g3
INCLUDES=-pthread -I./include/
LIBS=-lm -lrt -l38moths -loleg-http
NAME=waifu.xyz
COMMON_OBJ=benchmark.o blue_midnight_wish.o http.o models.o db.o parson.o utils.o


all: ctl bin downloader test $(NAME)

clean:
	rm -f *.o
	rm -f dbctl
	rm -f greshunkel_test
	rm -f unit_test
	rm -f downloader
	rm -f $(NAME)

test: unit_test
unit_test: $(COMMON_OBJ) server.o stack.o parse.o utests.o
	$(CC) $(CLAGS) $(LIB_INCLUDES) $(INCLUDES) -o unit_test $^ $(LIBS)

%.o: ./src/%.c
	$(CC) $(CFLAGS) $(LIB_INCLUDES) $(INCLUDES) -c $<

ctl: dbctl
dbctl: $(COMMON_OBJ) dbctl.o
	$(CC) $(CLAGS) $(LIB_INCLUDES) $(INCLUDES) -o dbctl $^ $(LIBS)

bin: $(NAME)
$(NAME): $(COMMON_OBJ) server.o main.o parson.o
	$(CC) $(CLAGS) $(LIB_INCLUDES) $(INCLUDES) -o $(NAME) $^ $(LIBS)

downloader: $(COMMON_OBJ) parse.o stack.o downloader.o
	$(CC) $(CLAGS) $(LIB_INCLUDES) $(INCLUDES) -o downloader $^ $(LIBS)
