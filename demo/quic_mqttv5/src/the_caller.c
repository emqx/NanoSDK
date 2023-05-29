
#include <stdio.h>
#include <unistd.h>

#include "emqx_interface.h"

#include "sample_data.h"

int
main(void)
{
   char *theURL = "mqtt-quic://54.183.144.230:14567";
   char *theCorrelation = "Just A Placeholder, for now.";
   char *theTopic = "Boc3HUvYZn";

   printf ("Setting up client connection.");
   fflush (stdout);

   void *clientConnection = setUpClient(theURL);

   //- Time for connection to set things up?
   usleep(1000000);

   for (int indx = 1000; indx; indx--) {
   	publishMessage(clientConnection, theTopic, theCorrelation, sampleJsonInOneLine);
   	usleep(5000);
   }

   printf ("Shutting down client connection.");
   fflush (stdout);
   shutdownClient(clientConnection);

   return 0;
}

