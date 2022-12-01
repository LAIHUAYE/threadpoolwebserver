
#include"http_conn.h"
const char* ok_200_title="ok";
const char* error_400_title="BAD REQUEST";
const char* error_400_form="YOUR REQUEST HAS BAD SYNTAX OR IS INHERENTLY IMPOSSIBLE TO SATISY\n";
const char *error_403_title = "forbidden";
const char* error_403_form="you do not have permission to get file from on this server\n";
const char* error_404_title="nof found";
const char* error_404_form="the requested file was not found on this server\n";
const char* error_500_title="internal error";
const char* error_500_form="there was an unusual problem serving the requested file\n";
const char* doc_root="/home/lhy/demo03_ws";/*root file*/
int setnonblocking(int fd){
    int old_option=fcntl(fd,F_GETFL);
    int new_option=old_option|O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}
void addfd(int epollfd,int fd,bool one_shot){
    epoll_event event;
    event.data.fd=fd;
    event.events=EPOLLIN|EPOLLET|EPOLLRDHUP;
    if(one_shot){
        event.events|=EPOLLONESHOT;
    }
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    setnonblocking(fd);

}
void removefd(int epollfd,int fd){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}
void modfd(int epollfd,int fd,int ev){
    epoll_event event;
    event.data.fd=fd;
    event.events=ev|EPOLLET|EPOLLONESHOT|EPOLLRDHUP;
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}
int http_conn::m_user_count=0;
int http_conn::m_epollfd=-1;
void http_conn::close_conn(bool real_close){
    if(real_close&&(m_sockfd!=-1)){
        removefd(m_epollfd,m_sockfd);
        m_sockfd=-1;
        m_user_count--;
    }
}
void http_conn::init(int sockfd,const sockaddr_in& addr){
    m_sockfd=sockfd;
    m_address=addr;
    addfd(m_epollfd,sockfd,true);
    m_user_count++;
    init();
}
void http_conn::init(){
    m_check_state=CHECK_STATE_REQUESTLINE;
    m_linger=false;
    m_method=GET;
    m_url=0;
    m_version=0;
    m_content_length=0;
    m_host=0;
    m_start_line=0;
    m_checked_idx=0;
    m_read_idx=0;
    m_write_idx=0;
    memset(m_read_buf,'\0',READ_BUFFER_SIZE);
    memset(m_write_buf,'\0',WRITE_BUFFER_SIZE);
    memset(m_real_file,'\0',FILENAME_LEN);
    printf("sockfd create\n");
}
http_conn::LINE_STATUS http_conn::parse_line(){
    char temp;
    for(;m_checked_idx<m_read_idx;++m_checked_idx){
        temp=m_read_buf[m_checked_idx];
        if(temp=='\r'){
            if((m_checked_idx+1)==m_read_idx){
                return LINE_OPEN;
            }
            else if(m_read_buf[m_checked_idx+1]='\n'){
                m_read_buf[m_checked_idx++]='\0';
                m_read_buf[m_checked_idx++]='\0';
                return LINE_OK;
            }
        }
        else if(temp=='\n'){
            if((m_checked_idx>1)&&(m_read_buf[m_checked_idx-1]=='\r')){
                m_read_buf[m_checked_idx++]='\0';
                m_read_buf[m_checked_idx++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}
bool http_conn::read(){
    if(m_read_idx>=READ_BUFFER_SIZE){
        return false;
    }
    int bytes_read=0;
    while (true)
    {
        bytes_read=recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
        if(bytes_read==-1){
            if(errno==EAGAIN||errno==EWOULDBLOCK){
                break;
            }
            return false;
        }
        else if(bytes_read==0){
            return false;
        }
        m_read_idx+=bytes_read;
    }
    return true;
}
http_conn::HTTP_CODE http_conn::parse_request_line(char*text){
    m_url=strpbrk(text,"\t");
    if(!m_url){
        return BAD_REQUEST;
    }
    *m_url++='\0';
    char* method=text;
    if(strcasecmp(method,"GET")==0){
        m_method=GET;
    }
    else{
        return BAD_REQUEST;
    }
    m_url+=strspn(m_url,"\t");
    m_version=strpbrk(m_url,"\t");
    if(!m_version){
        return BAD_REQUEST;
    }
    *m_version++='\0';
    m_version+=strspn(m_version,"\t");
    if(strcasecmp(m_version,"HTTP/1.1")!=0){
        return BAD_REQUEST;
    }
    if(strncasecmp(m_url,"http://",7)==0){
        m_url+=7;
        m_url=strchr(m_url,'/');
    }
    if(!m_url||m_url[0]!='/'){
        return BAD_REQUEST;
    }
    m_check_state=CHECK_STATE_HEADER;
    return NO_REQUEST;
}
http_conn::HTTP_CODE http_conn::parse_headers(char* text){
    if(text[0]=='\0'){
        if(m_content_length!=0){
            m_check_state=CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if(strncasecmp(text,"Connection:",11)==0){
        text+=11;
        text+=strspn(text,"\t");
        if(strcasecmp(text,"keep-alive")==0){
            printf("此次连接为长链接\n");
            m_linger=true;
        }

    }
    else if(strncasecmp(text,"Content-Length:",15)==0){
        text+=15;
        text+=strspn(text,"\t");
        m_content_length=atoi(text);
    }
    else if(strncasecmp(text,"Host:",5)==0){
        text+=5;
        text+=strspn(text,"\t");
        m_host=text;
    }
    else{
        printf("oop!unknow header%s\n",text);
    }
    return NO_REQUEST;
}
http_conn::HTTP_CODE http_conn::parse_content(char*text){
    if(m_read_idx>=(m_content_length+m_checked_idx)){
        text[m_content_length]='\0';
        return GET_REQUEST;
    }
    return  NO_REQUEST;
}
 http_conn::HTTP_CODE http_conn::process_read()
  {
      //初始化从状态机状态、HTTP请求解析结果
      LINE_STATUS line_status=LINE_OK;
      HTTP_CODE ret=NO_REQUEST;
      char* text= 0;
  
      //这里为什么要写两个判断条件？第一个判断条件为什么这样写？
      //具体的在主状态机逻辑中会讲解。
  
      //parse_line为从状态机的具体实现，提取出一行的内容
      while((m_check_state==CHECK_STATE_CONTENT && line_status==LINE_OK)||((line_status=parse_line())==LINE_OK))
      {
          text=get_line();
          
  
          //m_start_line是每一个数据行在m_read_buf中的起始位置
          //m_checked_idx表示从状态机在m_read_buf中读取的位置
          m_start_line=m_checked_idx;
          printf("got 1 http line:%s\n",text);
          //主状态机的三种状态转移逻辑
          switch(m_check_state)
          {
              case CHECK_STATE_REQUESTLINE:
              {
                  //解析请求行
                  ret=parse_request_line(text);
                  if(ret==BAD_REQUEST)
                      return BAD_REQUEST;
                  break;
              }
              case CHECK_STATE_HEADER:
              {
                  //解析请求头
                  ret=parse_headers(text);
                  if(ret==BAD_REQUEST)
                      return BAD_REQUEST;
  
                  //完整解析GET请求后，跳转到报文响应函数
                  else if(ret==GET_REQUEST)
                  {
                      return do_request();
                  }
                  break;
              }
              case CHECK_STATE_CONTENT:
              {
                  //解析消息体
                  ret=parse_content(text);
  
                  //完整解析POST请求后，跳转到报文响应函数
                  if(ret==GET_REQUEST)
                      return do_request();
  
                  //解析完消息体即完成报文解析，避免再次进入循环，更新line_status
                  line_status=LINE_OPEN;
                  break;
              }
              default:
              return INTERNAL_ERROR;
          }
      }
      return NO_REQUEST;
  }

http_conn::HTTP_CODE http_conn::do_request(){
    strcpy(m_real_file,doc_root);
    int len=strlen(doc_root);
    strncpy(m_real_file+len,m_url,FILENAME_LEN-len-1);
    if(stat(m_real_file,&m_file_stat)<0){
        return NO_RESOURCE;
    }
    if(!(m_file_stat.st_mode&S_IROTH)){
        return FORBIDDEN_REQUEST;
    }
    if(S_ISDIR(m_file_stat.st_mode)){
        return BAD_REQUEST;
    }
    int fd=open(m_real_file,O_RDONLY);
    m_file_address=(char*)mmap(0,m_file_stat.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    close(fd);
    return FILE_REQUEST;
}
void http_conn::unmap(){
    if(m_file_address){
        munmap(m_file_address,m_file_stat.st_size);
        m_file_address=0;
    }
}
//写HTTP响应
bool http_conn::write() {
	int temp = 0;
	int bytes_have_send = 0;
	int bytes_to_send = m_write_idx;
	if (bytes_to_send == 0) {
		//没东西可写，需要让m_sockfd去读
		modfd(m_epollfd, m_sockfd, EPOLLIN);
		init();
		return true;
	}

	while (1) {
		//writev以顺序iov[0]、iov[1]至iov[iovcnt-1]从各缓冲区中聚集输出数据到fd,减少了read和write的系统调用
		//readv则相反，将fd的内容一个一个填满iov[0],iov[1]...iov[count-1]
		temp = writev(m_sockfd, m_iv, m_iv_count);

		if (temp <= -1) {
			/*
				如果TCP写缓冲区没有空间，则等待下一轮EPOLLOUT事件。虽然在此期间，服务器无法立即接受到同一个客户的下一个请求，
				但这样可以保证连接的完整性
			*/
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				modfd(m_epollfd, m_sockfd, EPOLLOUT);
				return true;
			}
			unmap();
			return false;
		}
		bytes_to_send -= temp;
		bytes_have_send += temp;
		if (bytes_to_send <= bytes_have_send) {
			//发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否关闭连接
			unmap();
			if (m_linger) {
				init();
				modfd(m_epollfd, m_sockfd, EPOLLIN);
				return true;
			}
			else {
				modfd(m_epollfd, m_sockfd, EPOLLIN);
				return false;
			}

		}
	}
}


bool http_conn::add_response(const char* format,...)
{
    //如果写入内容超出m_write_buf大小则报错
    if(m_write_idx>=WRITE_BUFFER_SIZE)
        return false;

    //定义可变参数列表
    va_list arg_list;

    //将变量arg_list初始化为传入参数
    va_start(arg_list,format);

    //将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    int len=vsnprintf(m_write_buf+m_write_idx,WRITE_BUFFER_SIZE-1-m_write_idx,format,arg_list);

    //如果写入的数据长度超过缓冲区剩余空间，则报错
    if(len>=(WRITE_BUFFER_SIZE-1-m_write_idx)){
        va_end(arg_list);
        return false;
    }

    //更新m_write_idx位置
    m_write_idx+=len;
    //清空可变参列表
    va_end(arg_list);

    return true;
}

//添加状态行
bool http_conn::add_status_line(int status,const char* title)
{
    return add_response("%s %d %s\r\n","HTTP/1.1",status,title);
}

//添加消息报头，具体的添加文本长度、连接状态和空行
void http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}

//添加Content-Length，表示响应报文的长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n",content_len);
}


//添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n",(m_linger==true)?"keep-alive":"close");
}
//添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s","\r\n");
}

//添加文本content
bool http_conn::add_content(const char* content)
{
    return add_response("%s",content);
}

bool http_conn::process_write(HTTP_CODE ret){
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500,error_500_title);
        add_headers(strlen(error_500_form));
        if(!add_content(error_500_form)){return false;};
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(400,error_400_title);
        add_headers(strlen(error_400_form));
        if(!add_content(error_400_form))
        {return false;}
        break;
    }
    case NO_RESOURCE:
    {
        add_status_line(404,error_404_title);
        add_headers(strlen(error_404_form));
        if(!add_content(error_404_form)){return false;};
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403,error_403_title);
        add_headers(strlen(error_403_form));
        if(!add_content(error_403_form)){return false;};
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200,ok_200_title);
        if(m_file_stat.st_size!=0){
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base=m_write_buf;
            m_iv[0].iov_len=m_write_idx;
            m_iv[1].iov_base=m_file_address;
            m_iv[1].iov_len=m_file_stat.st_size;
            m_iv_count=2;
            return true;
        }
        else{
            const char* ok_string="<html><body></body></html>";
            add_headers(strlen(ok_string));
            if(!add_content(ok_string)){
                return false;
            }
        }
    }
    default:
        {
            return false;
        }
    }
    m_iv[0].iov_base=m_write_buf;
    m_iv[0].iov_len=m_write_idx;
    m_iv_count=1;
    return true;
}
void http_conn::process(){
    HTTP_CODE read_ret=process_read();
    if(read_ret==NO_REQUEST){
        printf("jixuduqu\n");
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        return;
    }
    bool write_ret=process_write(read_ret);
    if(!write_ret){
        printf("fasongshibai\n");
        close_conn();
    }
    modfd(m_epollfd,m_sockfd,EPOLLOUT);
}
