
EtherDream Strategy
-------------------

Buffer : 

The system will wait until at least the min packet size is available in the buffer 
Then ask for minimum of 0 and maximum of that packet size. 
The request function may not give you any points. 
In which case, wait for the maximum pause between requests and ask again. 
If it does give you points, and they're more than the minimum packet size, then send it, 
otherwise, save them and wait the maximum pause between requests. 

If the buffer is down to its minimum level, then the requester demands at least the min packet size of points. 

Frame latency isn't really handled at this level. 