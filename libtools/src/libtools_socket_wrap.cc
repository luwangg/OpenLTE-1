/*******************************************************************************

    Copyright 2013 Ben Wojtowicz

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*******************************************************************************

    File: libtools_socket_wrap.cc

    Description: Contains all the implementations for the text mode
                 communication socket abstraction tool.

    Revision History
    ----------    -------------    --------------------------------------------
    02/26/2013    Ben Wojtowicz    Created file

*******************************************************************************/

/*******************************************************************************
                              INCLUDES
*******************************************************************************/

#include "libtools_socket_wrap.h"
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

/*******************************************************************************
                              DEFINES
*******************************************************************************/


/*******************************************************************************
                              TYPEDEFS
*******************************************************************************/


/*******************************************************************************
                              GLOBAL VARIABLES
*******************************************************************************/


/*******************************************************************************
                              CLASS IMPLEMENTATIONS
*******************************************************************************/

// Constructor/Destructor
libtools_socket_wrap::libtools_socket_wrap(uint32                          *ip_addr,
                                           uint16                           port,
                                           LIBTOOLS_SOCKET_WRAP_TYPE_ENUM   type,
                                           void                             (*handle_msg)(std::string msg),
                                           void                             (*handle_connect)(void),
                                           void                             (*handle_disconnect)(void),
                                           void                             (*handle_error)(LIBTOOLS_SOCKET_WRAP_ERROR_ENUM err),
                                           LIBTOOLS_SOCKET_WRAP_ERROR_ENUM *error)
{
    boost::mutex::scoped_lock lock(socket_mutex);
    struct sockaddr_in        s_addr;
    struct linger             linger;

    if(handle_msg        == NULL ||
       handle_connect    == NULL ||
       handle_disconnect == NULL ||
       handle_error      == NULL)
    {
        *error = LIBTOOLS_SOCKET_WRAP_ERROR_INVALID_INPUTS;
        return;
    }

    // Save inputs
    socket_handle_msg        = handle_msg;
    socket_handle_connect    = handle_connect;
    socket_handle_disconnect = handle_disconnect;
    socket_handle_error      = handle_error;
    socket_port              = port;
    socket_state             = LIBTOOLS_SOCKET_WRAP_STATE_DISCONNECTED;

    // Create the socket
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&s_addr, 0, sizeof(s_addr));
    s_addr.sin_family      = AF_INET;
    s_addr.sin_addr.s_addr = INADDR_ANY;
    s_addr.sin_port        = htons(socket_port);
    if(0 > sock)
    {
        *error = LIBTOOLS_SOCKET_WRAP_ERROR_SOCKET;
        return;
    }

    if(LIBTOOLS_SOCKET_WRAP_TYPE_SERVER == type)
    {
        // Set the options
        linger.l_onoff  = 1;
        linger.l_linger = 0;
        if(0 != setsockopt(sock, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger)))
        {
            *error = LIBTOOLS_SOCKET_WRAP_ERROR_SOCKET;
            return;
        }
    }

    // Bind
    if(0 != bind(sock, (struct sockaddr *)&s_addr, sizeof(s_addr)))
    {
        *error = LIBTOOLS_SOCKET_WRAP_ERROR_SOCKET;
        return;
    }

    // Create read thread
    thread_inputs.socket_wrap       = this;
    thread_inputs.handle_msg        = socket_handle_msg;
    thread_inputs.handle_connect    = socket_handle_connect;
    thread_inputs.handle_disconnect = socket_handle_disconnect;
    thread_inputs.handle_error      = socket_handle_error;
    thread_inputs.sock              = sock;
    if(LIBTOOLS_SOCKET_WRAP_TYPE_SERVER == type)
    {
        if(0 != pthread_create(&socket_thread, NULL, &server_thread, &thread_inputs))
        {
            *error = LIBTOOLS_SOCKET_WRAP_ERROR_PTHREAD;
            return;
        }
    }else{
        if(0 != pthread_create(&socket_thread, NULL, &client_thread, &thread_inputs))
        {
            *error = LIBTOOLS_SOCKET_WRAP_ERROR_PTHREAD;
            return;
        }
    }
    socket_state = LIBTOOLS_SOCKET_WRAP_STATE_THREADED;

    *error = LIBTOOLS_SOCKET_WRAP_SUCCESS;
}
libtools_socket_wrap::~libtools_socket_wrap()
{
    boost::mutex::scoped_lock lock(socket_mutex);

    if(LIBTOOLS_SOCKET_WRAP_STATE_DISCONNECTED != socket_state)
    {
        socket_handle_disconnect();
        socket_state = LIBTOOLS_SOCKET_WRAP_STATE_DISCONNECTED;
    }
    close(sock);
    pthread_cancel(socket_thread);
    pthread_join(socket_thread, NULL);
}

// Send
LIBTOOLS_SOCKET_WRAP_ERROR_ENUM libtools_socket_wrap::send(std::string msg)
{
    boost::mutex::scoped_lock       lock(socket_mutex);
    LIBTOOLS_SOCKET_WRAP_ERROR_ENUM err = LIBTOOLS_SOCKET_WRAP_ERROR_SOCKET;

    if(LIBTOOLS_SOCKET_WRAP_STATE_CONNECTED == socket_state)
    {
        if(msg.size() != write(socket_fd, msg.c_str(), msg.size()))
        {
            err = LIBTOOLS_SOCKET_WRAP_ERROR_WRITE_FAIL;
        }else{
            err = LIBTOOLS_SOCKET_WRAP_SUCCESS;
        }
    }

    return(err);
}

// Receive
void* libtools_socket_wrap::server_thread(void *inputs)
{
    std::string                                msg;
    LIBTOOLS_SOCKET_WRAP_THREAD_INPUTS_STRUCT *act_inputs = (LIBTOOLS_SOCKET_WRAP_THREAD_INPUTS_STRUCT *)inputs;
    struct sockaddr_in                         c_addr;
    socklen_t                                  c_addr_len;
    LIBTOOLS_SOCKET_WRAP_STATE_ENUM            state;
    int32                                      sock_fd;
    int32                                      nbytes;
    int32                                      ready;
    char                                       read_buf[LINE_MAX];

    // Put socket into listen mode
    if(0 != listen(act_inputs->sock, 1))
    {
        act_inputs->handle_error(LIBTOOLS_SOCKET_WRAP_ERROR_SOCKET);
        return(NULL);
    }

    while(1)
    {
        // Accept connections
        sock_fd = accept(act_inputs->sock, (struct sockaddr *)&c_addr, &c_addr_len);
        if(0 > sock_fd)
        {
            act_inputs->handle_error(LIBTOOLS_SOCKET_WRAP_ERROR_SOCKET);
            return(NULL);
        }

        // Finish setup
        act_inputs->socket_wrap->socket_mutex.lock();
        act_inputs->socket_wrap->socket_fd    = sock_fd;
        act_inputs->socket_wrap->socket_state = LIBTOOLS_SOCKET_WRAP_STATE_CONNECTED;
        act_inputs->socket_wrap->socket_mutex.unlock();
        act_inputs->handle_connect();

        // Read loop
        while(1)
        {
            memset(read_buf, '\0', LINE_MAX);
            nbytes = read(sock_fd, read_buf, LINE_MAX);

            // Check the return
            if(-1 == nbytes)
            {
                state = LIBTOOLS_SOCKET_WRAP_STATE_CLOSED;
                break;
            }else if(0 == nbytes){
                state = LIBTOOLS_SOCKET_WRAP_STATE_THREADED;
                break;
            }else{
                // Remove \n
                read_buf[strlen(read_buf)-1] = '\0';
                msg = read_buf;

                // Check for \r
                if(std::string::npos != msg.find("\r"))
                {
                    // Remove \r
                    read_buf[strlen(read_buf)-1] = '\0';
                    msg = read_buf;

                    // Process input
                    while(std::string::npos != msg.find("\n"))
                    {
                        act_inputs->handle_msg(msg.substr(0, msg.find("\n")-1));
                        msg = msg.substr(msg.find("\n")+1, std::string::npos);
                    }
                    act_inputs->handle_msg(msg);
                }else{
                    // Process input
                    while(std::string::npos != msg.find("\n"))
                    {
                        act_inputs->handle_msg(msg.substr(0, msg.find("\n")));
                        msg = msg.substr(msg.find("\n")+1, std::string::npos);
                    }
                    act_inputs->handle_msg(msg);
                }
            }
        }

        if(LIBTOOLS_SOCKET_WRAP_STATE_CLOSED == state)
        {
            break;
        }
    }

    act_inputs->socket_wrap->socket_mutex.lock();
    act_inputs->socket_wrap->socket_state = state;
    act_inputs->socket_wrap->socket_mutex.unlock();

    // Close
    close(sock_fd);

    return(NULL);
}
void* libtools_socket_wrap::client_thread(void *inputs)
{
    // FIXME
    return(NULL);
}
