all: sender receiver

sender: udp_sender.c
	gcc -Wall -Werror -o udp_sender udp_sender.c

receiver: udp_receiver.c
	gcc -Wall -Werror -o udp_receiver udp_receiver.c

clean: 
	-rm udp_sender udp_receiver

