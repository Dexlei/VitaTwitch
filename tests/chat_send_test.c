#include "chat.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
 char out[CHAT_TEXT_MAX];char long_msg[465];
 assert(chat_prepare_outgoing("  Ola Twitch  ",out,sizeof out)==10);
 assert(strcmp(out,"Ola Twitch")==0);
 assert(chat_prepare_outgoing(" Olá 世界 ",out,sizeof out)>0);
 assert(chat_prepare_outgoing("\r\nPRIVMSG #other :injected",out,sizeof out)<0);
 assert(chat_prepare_outgoing("Hello\nJOIN #other",out,sizeof out)<0);
 assert(chat_prepare_outgoing("   ",out,sizeof out)<0);
 memset(long_msg,'a',sizeof long_msg-1);long_msg[sizeof long_msg-1]=0;
 assert(chat_prepare_outgoing(long_msg,out,sizeof out)<0);
 assert(chat_prepare_outgoing("OK",out,2)<0);
 assert(chat_prepare_outgoing(" OK ",out,sizeof out)==2);
 puts("PASS: chat IRC payload validation, unicode, byte bounds, CRLF injection");
 return 0;
}
