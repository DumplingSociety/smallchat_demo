all: smallchat

smallchat: smallchat.c
	$(CC) smallchat.c -o smallchat -O2 -Wall -W -g

clean:
	rm -f smallchat
