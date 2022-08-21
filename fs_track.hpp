#define MAX_EVENTS 1024
#define LEN_NAME 16
#define EVENT_SIZE  ( sizeof (struct inotify_event) )
#define BUF_LEN     ( MAX_EVENTS * ( EVENT_SIZE + LEN_NAME ))

 #include <sys/inotify.h>
 #include <set>

extern std::set<std::string> dir_cache;

int watchingInit(const char* p)
{
    int length, i = 0, wd;
    int fd;
    char buffer[BUF_LEN];

    fd = inotify_init1(0);
    if ( fd < 0 ) {
        perror( "Couldn't initialize inotify");
    }
    const char* path = p;

    wd = inotify_add_watch(fd, path, IN_CLOSE );

    if (wd == -1)
    {
        printf("Couldn't add watch to %s\n",path);
    }
    else
    {
        printf("Watching:: %s\n",path);
    }
    while(1)
    {
        i = 0;
        length = read( fd, buffer, BUF_LEN );
        if ( length < 0 ) {
            perror( "read" );
        }

        while ( i < length ) {
			auto *event = ( struct inotify_event * ) &buffer[ i ];
			if ( event->len ) {
				if ( event->mask & IN_CLOSE )
				{
						dir_cache.insert(event->name);
				}
					i += EVENT_SIZE + event->len;
				}
        }
    }
    inotify_rm_watch( fd, wd );
    close( fd );
    return 0;
}
