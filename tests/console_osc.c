#include "console.h"
#include <stdio.h>
#include <string.h>
int main(void) {
 console c; char text[1024]; console_init(&c,80,10);
 const char *parts[]={"\033]8;;https://example.test/\033","\\status\033]8;;\033\\\r\nlocalhost login:",0};
 for(int i=0;parts[i];i++)console_put_bytes(&c,(const unsigned char*)parts[i],strlen(parts[i]));
 console_text_snapshot(&c,text,sizeof text);
 if(!strstr(text,"status")||!strstr(text,"localhost login:")||strstr(text,"example")){fprintf(stderr,"FAIL ST snapshot: %s\n",text);return 1;}
 console_reset(&c);const char *bel="\033]title\007BEL ok";console_put_bytes(&c,(const unsigned char*)bel,strlen(bel));console_text_snapshot(&c,text,sizeof text);
 if(!strstr(text,"BEL ok")){return 2;}
 puts("PASS OSC BEL and split ESC-backslash preserve following login text");console_free(&c);return 0;
}
