/*
* @brief     进程函数，逻辑主体
* @details   包含代理进程函数以及工作线程函数
*/
#include "../header/tcp_proxy.h"

#define OPEN_MAX 20480
#define MAX_LINE 1024



pthread_mutex_t conn_mutex = PTHREAD_MUTEX_INITIALIZER;

void completion_handler_write(struct event *event_info);
void completion_handler_read(struct event *event_info);


void proxy_epoll(struct proxy *proxy_info,struct statistics * statistics_info)
{

    int client_num=0;
    int listen_fd,client_fd;
    int res = 0;
    int ep_fd,n_ready,n_i;
    int recv_len = 0;

    struct epoll_event tep,ep_event[OPEN_MAX];
    struct event *event_tmp,*event_info = (struct event *)malloc(sizeof(struct event));
    listen_fd = create_listenfd(proxy_info->listen_port);

    ep_fd = epoll_create(OPEN_MAX);
    if(ep_fd == -1)
    {
        printf("epoll create erroe\n");
        exit(1);
    }
    event_info->src_fd = listen_fd;
    event_info->dst_fd = 0;
    tep.events = EPOLLIN;
    tep.data.ptr = (void *)event_info;

    res = epoll_ctl(ep_fd,EPOLL_CTL_ADD,listen_fd,&tep);
    if(res == -1)
    {
        printf("epoll_ctl add root error\n");
    }
    for( ; ; )
    {
        n_ready = epoll_wait(ep_fd, ep_event,OPEN_MAX,-1);
        if(n_ready == -1)
        {
            printf("epoll_wait error\n");
        }
        for(n_i = 0; n_i < n_ready; ++n_i)
        {
            if(!(ep_event[n_i].events & EPOLLIN))
                continue;
            event_tmp = (struct event*)ep_event[n_i].data.ptr;
            if(event_tmp->src_fd == listen_fd)  //监听描述符有新请求
            {
                res = accept_and_conn(ep_fd, listen_fd, proxy_info,statistics_info);
                if(res == -1)
                {
                    printf("accept_and_conn error\n");
                }
            } 
            else
            {
                res = info_transmit(event_tmp);
                if(res == -1)
                {
                    printf("trans error\n");
                }
            }
            
        }
    }
}

static void refresh_data_info(int c_s,int len,struct statistics * statistics_info)
{
    if (0 == c_s)
    {
        pthread_mutex_lock(&(statistics_info->request_data_mutex));
        statistics_info->client_to_proxy_data += len;
        statistics_info->proxy_to_server_data += len;
        pthread_mutex_unlock(&(statistics_info->request_data_mutex));
    }
    else
    {
        pthread_mutex_lock(&(statistics_info->response_data_mutex));
        statistics_info->proxy_to_client_data += len;
        statistics_info->server_to_proxy_data += len;
        pthread_mutex_unlock(&(statistics_info->response_data_mutex));
    }
}

/**
 * func : epoll事件aio初始化
 * return value : 
 *              success : 1 ;
 *              error   : -1 ;
 */ 
int event_init(struct event * event_info)
{
    /* read aio init */
    event_info->r_cb = (struct aiocb*)malloc(sizeof(struct aiocb));
    if(event_info->r_cb == NULL)
    {
        printf(" event_info->r_cb allocate error\n ");
        return -1;
    }
    bzero(event_info->r_cb, sizeof(struct aiocb));
    event_info->r_cb->aio_buf = (char *)malloc(sizeof(char) * MAX_LINE);
    if(event_info->r_cb->aio_buf == NULL)
    {
        printf(" event_info->r_cb->aio_buf allocate error\n ");
        return -1;
    }
    event_info->r_cb->aio_fildes = event_info->src_fd;
    event_info->r_cb->aio_nbytes = MAX_LINE;
    event_info->r_cb->aio_offset = 0;
    event_info->r_cb->aio_sigevent.sigev_notify = SIGEV_THREAD;
    event_info->r_cb->aio_sigevent.sigev_notify_function = completion_handler_read;
    event_info->r_cb->aio_sigevent.sigev_notify_attributes = NULL;
    event_info->r_cb->aio_sigevent.sigev_value.sival_ptr = event_info;

    /* write aio init */
    event_info->w_cb = (struct aiocb*)malloc(sizeof(struct aiocb));
    if(event_info->w_cb == NULL)
    {
        printf(" event_info->w_cb allocate error\n ");
        return -1;
    }
    bzero(event_info->w_cb, sizeof(sizeof(struct aiocb)));
    event_info->w_cb->aio_buf = (char *)malloc(sizeof(char) * MAX_LINE);
    if(event_info->w_cb->aio_buf == NULL)
    {
        printf(" event_info->w_cb->aio_buf allocate error\n ");
        return -1;
    }
    event_info->w_cb->aio_fildes = event_info->dst_fd;
    event_info->w_cb->aio_nbytes = MAX_LINE;
    event_info->w_cb->aio_offset = 0;

    event_info->w_cb->aio_sigevent.sigev_notify = SIGEV_THREAD;
    event_info->w_cb->aio_sigevent.sigev_notify_function = completion_handler_write;
    event_info->w_cb->aio_sigevent.sigev_notify_attributes = NULL;
    event_info->w_cb->aio_sigevent.sigev_value.sival_ptr = event_info;
    return 1;
}

int accept_and_conn(int ep_fd, int listen_fd, struct proxy *proxy_info,struct statistics * statistics_info)
{
    int client_fd,server_fd; /* 客户端，服务器  文件描述符*/
    int res;

    struct sockaddr_in client_addr, server_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    struct epoll_event *client_event = (struct epoll_event *)malloc(sizeof(struct epoll_event));
    struct epoll_event *server_event = (struct epoll_event *)malloc(sizeof(struct epoll_event));
    struct event * client_event_info = (struct event *)malloc(sizeof(struct event));
    struct event * server_event_info = (struct event *)malloc(sizeof(struct event));

    /* 接受客户端请求 */
    client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_addr_len);
    /* 互斥更新连接信息 */
    pthread_mutex_lock(&(statistics_info->client_proxy_connections_now_mutex));
    statistics_info->client_proxy_CPS++;
    statistics_info->client_proxy_connections_all++;
    pthread_mutex_unlock(&(statistics_info->client_proxy_connections_now_mutex));

    pthread_mutex_lock(&(statistics_info->client_proxy_connections_now_mutex));
    statistics_info->client_proxy_connections_now++;
    pthread_mutex_unlock(&(statistics_info->client_proxy_connections_now_mutex));

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1)
    {
        printf("socket to server error\n");
        return -1;
    }
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(proxy_info->server_port);
    server_addr.sin_addr.s_addr = inet_addr(proxy_info->server_ip);

    /* 连接服务器 */
    if (connect(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        printf("connect error\n");
        return -1;
    }
    
    //互斥访问代理程序与服务器每秒建立连接数以及总连接数
    pthread_mutex_lock(&(statistics_info->proxy_server_connections_mutex));
    statistics_info->proxy_server_CPS++;
    statistics_info->proxy_server_connections_all++;
    pthread_mutex_unlock(&(statistics_info->proxy_server_connections_mutex));

    //互斥访问正在处理的代理程序与服务器连接数
    pthread_mutex_lock(&(statistics_info->proxy_server_connections_now_mutex));
    statistics_info->proxy_server_connections_now++;
    pthread_mutex_unlock(&(statistics_info->proxy_server_connections_now_mutex));

    /* 客户端epoll监听 */
    client_event_info->epoll_fd = ep_fd;
    client_event_info->src_fd = client_fd;
    client_event_info->dst_fd = server_fd;
    client_event_info->c_s = 0;
    client_event_info->statistics_info = statistics_info;
    client_event_info->epoll_event_src = client_event;
    client_event_info->epoll_event_dst = server_event;
    if( event_init(client_event_info) == -1)
    {
        printf("event_init client error\n");
    }

    
    client_event->events = EPOLLIN;
    client_event->data.ptr = (void *)client_event_info;


    res = epoll_ctl(ep_fd, EPOLL_CTL_ADD, client_fd, client_event);
    if (res == -1)
    {
        printf("epoll add client error\n");
        return -1;
    }
    /* 服务器epoll监听 */
    server_event_info->epoll_fd = ep_fd;
    server_event_info->src_fd = server_fd;
    server_event_info->dst_fd = client_fd;
    server_event_info->c_s = 1;
    server_event_info->statistics_info = statistics_info;
    server_event_info->epoll_event_src = server_event;
    server_event_info->epoll_event_dst = client_event;
    if( event_init(server_event_info) == -1)
    {
        printf("event_init server error\n");
    }


    server_event->events = EPOLLIN;
    server_event->data.ptr = (void *)server_event_info;
    res = epoll_ctl(ep_fd, EPOLL_CTL_ADD, server_fd, server_event);
    if (res == -1)
    {
        printf("epoll add server error\n");
        return -1;
    }
    //printf("conn ect\n");
    return 0;
}

void completion_handler_write(struct event *event_info)
{
    /* 一次转发结束 */
    int res = epoll_ctl(event_info->epoll_fd,EPOLL_CTL_ADD,event_info->src_fd,event_info->epoll_event_src);
}

void completion_handler_read(struct event *event_info)
{
    /* 读取内容结束，转发至对端 */
    int res = 0,length = 0;
    //printf("read : %s\n",event_info->r_cb->aio_buf);
    res = aio_error(event_info->r_cb);
    printf("request length : %d\n",strlen((char *)event_info->r_cb->aio_buf));
    if( res == 0)
    {
        length = aio_return(event_info->r_cb);
        printf("length: %d\n",length);
    }
    if(length > 0)
    {
        strcpy((char *)event_info->w_cb->aio_buf, (char *)event_info->r_cb->aio_buf);
        bzero((char *)event_info->r_cb->aio_buf, sizeof(char) * MAX_LINE);
        //res = epoll_ctl(event_info->epoll_fd, EPOLL_CTL_ADD, event_info->src_fd, event_info->epoll_event);
        if (res == -1)
        {
            printf("epoll_del error in info_transmit\n");
        }
        //event_info->w_cb->aio_nbytes = strlen((char *)event_info->w_cb->aio_buf);
        event_info->w_cb->aio_nbytes = length;
        res = aio_write(event_info->w_cb);
        if (res != 0)
        {
            printf("aio_read error in func info_transmit\n");
        }
    } else if (length == 0)
    {
        /* destroy  */
        res = epoll_ctl(event_info->epoll_fd,EPOLL_CTL_DEL,event_info->src_fd,NULL);
        res = epoll_ctl(event_info->epoll_fd,EPOLL_CTL_DEL,event_info->dst_fd,NULL);
        close(event_info->src_fd);
        close(event_info->dst_fd);
        printf("close\n");
    }else
    {
        printf("read error\n");
    }
    
    
}

int info_transmit(struct event *trans_event)
{
    int res;
    //printf("ep: %d : fd : %d\n",trans_event->epoll_fd,trans_event->src_fd);
    res = epoll_ctl(trans_event->epoll_fd,EPOLL_CTL_DEL,trans_event->src_fd,trans_event->epoll_event_src);
    if(res == -1)
    {
        printf("epoll_del error in info_transmit\n");
    }
    res = aio_read(trans_event->r_cb);
    if(res != 0)
    {
        printf("aio_read error in func info_transmit\n");
    }
}

// int info_transmit(int ep_fd, struct event *trans_event,struct statistics * statistics_info)
// {
//     int len;
//     char buffer[MAX_LINE];
//     len = recv(trans_event->epoll_fd,buffer,sizeof(buffer),0);

//     //refresh_data_info(trans_event->c_s,len,statistics_info);

//     if(len > 0)
//     {
//         len = send(trans_event->dst_fd, buffer, len, 0);
//         refresh_data_info(trans_event->c_s,len,statistics_info);
//         //printf("%s\n",trans_event->buffer);
//         return 1;
//     }
//     else if(len == 0)
//     {
//         //printf("client closed\n");
//         epoll_ctl(ep_fd, EPOLL_CTL_DEL, trans_event->epoll_fd, NULL);
//         epoll_ctl(ep_fd, EPOLL_CTL_DEL, trans_event->dst_fd, NULL);
//         //互斥统计完成连接数
//         pthread_mutex_lock(&(statistics_info->connections_finished_mutex));
//         statistics_info->client_proxy_connections_finished++;
//         statistics_info->proxy_server_connections_finished++;
//         pthread_mutex_unlock(&(statistics_info->connections_finished_mutex));

//         pthread_mutex_lock(&(statistics_info->client_proxy_connections_now_mutex));
//         statistics_info->client_proxy_connections_now--;
//         pthread_mutex_unlock(&(statistics_info->client_proxy_connections_now_mutex));

//         pthread_mutex_lock(&(statistics_info->proxy_server_connections_now_mutex));
//         statistics_info->proxy_server_connections_now--;
//         pthread_mutex_unlock(&(statistics_info->proxy_server_connections_now_mutex));
//         close(trans_event->epoll_fd);
//         close(trans_event->dst_fd);
//         return 1;
//     }
//     else
//     {
//         printf("recv error\n");
//         return -1;
//     }
// }