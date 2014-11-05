/*
  Sample solution for NOS 2014 assignment: implement a simple multi-threaded 
  IRC-like chat service.

  (C) Paul Gardner-Stephen 2014.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
//#include <sys/filio.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <netdb.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <ctype.h>

struct client_thread {
  pthread_t thread;
  int thread_id;
  int fd;

  char nickname[32];

  int state;
#define DEAD 1
#define ALIVE 2
  int user_command_seen;
  int user_has_registered;
  time_t timeout;

  char line[1024];
  int line_len;

  int next_message;
};

// allocates the structure for the connections
#define MAX_CLIENTS 1000
struct client_thread threads[MAX_CLIENTS];
// the number of connections we have open right now
int client_count=0;

pthread_rwlock_t message_log_lock = PTHREAD_RWLOCK_INITIALIZER;

#define MAX_MESSAGES 10000
char *message_log_senders[MAX_MESSAGES];
char *message_log_recipients[MAX_MESSAGES];
char *message_log[MAX_MESSAGES];
int message_count=0;

int message_log_append(char *sender,
		       char *recipient,
		       char *message) {
  if (message_count>=MAX_MESSAGES) return -1;
  pthread_rwlock_wrlock(&message_log_lock);

  // Append the message to the log here
  message_log_senders[message_count]=strdup(sender);
  message_log_recipients[message_count]=strdup(recipient);
  message_log[message_count]=strdup(message);
  message_count++;

  pthread_rwlock_unlock(&message_log_lock);
  return 0;
}

int message_log_read(struct client_thread *t) {
  pthread_rwlock_rdlock(&message_log_lock);

  // Read and process new messages in the log
  int i;
  for (i=t->next_message;i<message_count;++i) {
    if (!strcasecmp(t->nickname, message_log_recipients[i])) {
      // This message is for us
      char msg[8192];
      snprintf(msg, 8192, ":%s PRIVMSG %s :%s\n",
	       message_log_senders[i],
	       message_log_recipients[i],
	       message_log[i]);
      write(t->fd,msg,strlen(msg));
    }
  }
  t->next_message=message_count;

  pthread_rwlock_unlock(&message_log_lock);
  return 0;

}

int read_from_socket(int sock,unsigned char *buffer,int *count,int buffer_size,
		     int timeout)
{
  fcntl(sock,F_SETFL,fcntl(sock, F_GETFL, NULL)|O_NONBLOCK);


  int t=time(0)+timeout;
  if (*count>=buffer_size) return 0;
  int r=read(sock,&buffer[*count],buffer_size-*count);
  while(r!=0) {
    if (r>0) {
      (*count)+=r;
      break;
    }
    r=read(sock,&buffer[*count],buffer_size-*count);
    if (r==-1&&errno!=EAGAIN) {
      perror("read() returned error. Stopping reading from socket.");
      return -1;
    } else usleep(100000);
    // timeout after a few seconds of nothing
    if (time(0)>=t) break;
  }
  buffer[*count]=0;
  return 0;
}

int create_listen_socket(int port)
{
  int sock = socket(AF_INET,SOCK_STREAM,0);
  if (sock==-1) return -1;

  int on=1;
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on)) == -1) {
    close(sock); return -1;
  }
  if (ioctl(sock, FIONBIO, (char *)&on) == -1) {
    close(sock); return -1;
  }
  
  /* Bind it to the next port we want to try. */
  struct sockaddr_in address;
  bzero((char *) &address, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(port);
  if (bind(sock, (struct sockaddr *) &address, sizeof(address)) == -1) {
    close(sock); return -1;
  } 

  if (listen(sock, 20) != -1) return sock;

  close(sock);
  return -1;
}

int accept_incoming(int sock)
{
  struct sockaddr addr;
  unsigned int addr_len = sizeof addr;
  int asock;
  if ((asock = accept(sock, &addr, &addr_len)) != -1) {
    return asock;
  }

  return -1;
}

int connection_count=0;

int connection_made(int fd);

void *client_thread_entry(void *arg) {
  struct client_thread *t=arg;

  //printf("Client thread started\n");
  // run the thread stuffs
  connection_main(t);
  t->state=DEAD;
  return NULL;
}

int handle_connection(int fd) {

  int i;
  for (i=0;i<client_count;++i) { if (threads[i].state==DEAD) break; }

  

  if (client_count>=MAX_CLIENTS) {
    write(fd, "ERROR :Closing Link: Too many connections\n", 42);
    close(fd);
    return 0;
  }
  
  // ADD STRUCTURE CLEARING LINE HERE
  // MARK THE CONNECTION SLOTS AS FREE FOR REUSE WHEN A CONNECTION CLOSES
  bzero(&threads[i],sizeof(struct client_thread));
  
  threads[i].fd = fd;
  threads[i].state = ALIVE;
  threads[i].thread_id = i;
  if (pthread_create(&threads[i].thread, NULL,
		     client_thread_entry, &threads[i])) {
    close(fd);
    return 0;
  }

  if (i==client_count) ++client_count;

  return 0;
}

int registration_check(struct client_thread *t) {
  if (t->user_has_registered) return -1;

  if (t->user_command_seen&&t->nickname[0]) {
    // User has now met the registration requirements
    t->user_has_registered=1;
    t->timeout=60;
    char msg[8192];
    snprintf(msg, 1024, ":toddsircserver.com 001 %s :You have registered. Hey there.\n", t->nickname);
    write(t->fd,msg,strlen(msg));
    snprintf(msg, 1024, ":toddsircserver.com 002 %s :More information to go here.\n", t->nickname);
    write(t->fd,msg,strlen(msg));
    snprintf(msg, 1024, ":toddsircserver.com 003 %s :This message intentionally left blank.\n", t->nickname);
    write(t->fd,msg,strlen(msg));
    snprintf(msg, 1024, ":toddsircserver.com 004 %s :Be generous with marks.\n", t->nickname);
    write(t->fd,msg,strlen(msg));
    snprintf(msg, 1024, ":toddsircserver.com 253 %s :??? unknown connections.\n", t->nickname);
    write(t->fd,msg,strlen(msg));
    snprintf(msg, 1024, ":toddsircserver.com 254 %s :??? channels formed.\n", t->nickname);
    write(t->fd,msg,strlen(msg));
    snprintf(msg, 1024, ":toddsircserver.com 255 %s :I have ??? clients and ??? servers.\n", t->nickname);
    write(t->fd,msg,strlen(msg));
    return 0;
  }
  return -1;
}

int parse_line(struct client_thread *t,char *buffer) {
  char msg[1024];
  char channel[8192];
  int r=sscanf((char *)buffer, "JOIN %s", channel);
  if (r==1) {
    if (!t->user_has_registered) {
      snprintf(msg, 1024, ":toddsircserver.com 241 * :JOIN command sent before registration\n");
      write(t->fd, msg, strlen(msg));
    } 
  }
  char nickname[8192];
  r=sscanf((char *)buffer, "NICK %s", nickname);
  if (r==1) {
    // Saw a nickname from the client
    if (strlen(nickname)>30||strlen(nickname)<1) {
      snprintf(msg,1024,"ERROR :Invalid nickname: Nickname too short or too long\n");
      write(t->fd,msg,strlen(msg));
    }
    else {
      // Nickname is a valid length
      //printf("Saw nickname '%s'\n",nickname);
      strcpy(t->nickname,nickname);
      registration_check(t);
    }
  }
  if (!strncasecmp("USER ", (char *)buffer,5)) {
    // Saw USER command. If a NICK has been provided, then mark connection
    // as registered and send client greeting messages.
    t->user_command_seen=1;
    registration_check(t);
  }
  if (!strncasecmp("PRIVMSG", (char *)buffer, 7)) {
    if (!t->user_has_registered) {
      snprintf(msg, 1024, ":toddsircserver.com 241 * :PRIVMSG command sent before registration\n");
      write(t->fd, msg, strlen(msg));
    }
    else {
      // Accept and process PRIVMSG
      char recipient[1024];
      char message[1024];
      if (sscanf((char *)buffer, "PRIVMSG %s :%[^\n]", recipient, message)==2) {
	char sender[1024];
	snprintf(sender,1024,"%s!myusername@myserver",t->nickname);
	message_log_append(sender, recipient, message);
      }
      else {
	// mal-formed PRIVMSG command, so complain
	snprintf(msg, 1024, ":toddsircserver.com 461 %s :Mal-formed PRIVMSG command sent\n", t->nickname);
	write(t->fd, msg, strlen(msg));
      }
    }
  }
  if (!strncasecmp("QUIT", (char *)buffer, 4)) {
    // Client is leaving
    // if we don't close connection, there will be a SIGPIPE.
    // be afraid.
    snprintf(msg, 1024, "ERROR :Closing Link: Client sent QUIT\n");
    write(t->fd, msg, strlen(msg));
    close(t->fd); t->fd=-1;
    return 0;
  }
  
}

int connection_main(struct client_thread *t) {
  //printf("I have now seen %d connections.\n", ++connection_count);
  char msg[8192];
  sprintf(msg, ":toddsircserver.com 020 * :Heyooooo\n");
  write(t->fd, msg, strlen(msg));

  unsigned char buffer[8192];
  int length=0;
  t->timeout=5;

  int time_of_last_data=time(0);
  while(t->fd>=0) {
    length=0;
    message_log_read(t);
    read_from_socket(t->fd, buffer, &length, 8192, 1);
    if (length>0) time_of_last_data=time(0);
    if (length==0&&((time(0)-time_of_last_data)>=t->timeout)) {
      snprintf(msg, 1024, "ERROR :Closing Link: Connection timeout\n");
      write(t->fd, msg, strlen(msg));
      close(t->fd);
      return 0;
    }

    buffer[length]=0;

    char line[1024];
    int line_length=0;
    int i;
    for (i=0;i<length;++i) {
      if (buffer[i]=='\n'||buffer[i]=='\r') {
	parse_line(t,line);
	line_length=0; line[0]=0;
      }
      else {
	if (line_length<1024) {
	  line[line_length++]=buffer[i];
	  line[line_length]=0;
	}
      }
    }
    if (line_length>0) parse_line(t,line);
  }

  close(t->fd);
  return 0;
}

int main(int argc,char **argv)
{
  signal(SIGPIPE, SIG_IGN);

  if (argc!=2) {
  fprintf(stderr,"usage: sample <tcp port>\n");
  exit(-1);
  }
  
  int master_socket = create_listen_socket(atoi(argv[1]));
  
  fcntl(master_socket,F_SETFL,fcntl(master_socket, F_GETFL, NULL)&(~O_NONBLOCK));  
  
  while(1) {
    int client_sock = accept_incoming(master_socket);
    if (client_sock!=-1) {
      handle_connection(client_sock);
    }
    else usleep(10000);
  }
}
