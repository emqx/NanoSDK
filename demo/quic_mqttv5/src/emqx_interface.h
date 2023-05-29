
void *setUpClient(const char *theURL);

void publishMessage(void *emqxClient, char *theTopic, char *theCorrelation, char *theData);

void shutdownClient (void *emqxClient);

