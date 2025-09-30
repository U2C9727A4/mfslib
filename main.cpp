#define OP_NOOP 0
#define OP_READ 1
#define OP_WRITE 2
#define OP_LS 3
#define OP_ERROR 4
#define RESPONSE_OF(x) ((x) | 0x80)
#define MFS_RESERVED_OP_RANGE 30

// An empty client's fd is always 0.
typedef unsigned int client_t;


typedef struct {
    client_t client;
    unsigned long long timer_end;

} client_handlers_t;

typedef struct {
    unsigned int psize;
    unsigned int dsize;
    unsigned char op;

    char* path;
    char* data;
} mfs_message_t;

// POSIX style write, read and close functions.
typedef long long(*write_cb)(client_t, char*, unsigned long long);
typedef long long(*read_cb)(client_t, char*, unsigned long long);
typedef void (*close_cb)(client_t);
typedef void (*setup_cb)(mfs_message_t);
typedef unsigned long long (*available_cb)(client_t);
typedef client_t (*accept_cb)(void);
typedef unsigned long long (*get_time_cb)();

/*
    MANUAL OF CALLBACKS
    writecb returns how much data was written, -1 on error. the first arguement is the client identifier, second arguement is the buffer and the third is the size of the buffer.
    read_cb is the same as writecb except it reads into the buffer and returns how many bytes it read. returns -1 on error.
    close_cb takes in the client identifier and closes the connection.
    setup_cb handles the setup mode of the MFS protocol.
    available_cb returns how much data (in bytes) is available from the client. **Should return 0 if the client's client_t is zero.**
    accept_cb accepts a new client to connect, returns 0 if theres no new clients.
    get_time_cb returns the current time since the MCU has started in milliseconds. (This is equivelent to the `millis()` function in arduino.)

    All of these functions should block until their tasks are finished, However it is recommended for implementors of these functions to make them time-out after the operation takes too long.
    The reason for this is their blocking nature, If the function blocks indefinitely, then the MCU would be deadlocked. and malicious clients could for example, send the headers of an MFS message, but never write the actual data and path
    causing the MFS server and the MCU to deadlock waiting for it to write.
*/

// File callback functions
// These functions are designated to be called by the corresponding file, and thus are responsible for returning an MFS message to send back to the client.
typedef mfs_message_t (*fwrite_t)(mfs_message_t);
typedef mfs_message_t (*fread_t)(mfs_message_t);


// All of the fields should be zero if its empty.
typedef struct {
    char* path; // The path should be a NULL-terminated C string. It should be NULL if the file is empty.
    unsigned int path_size; // Size of the path buffer NOT THE LENGHT OF THE STRING!

    fwrite_t writer_f;
    fread_t reader_f;
} mfs_file_t;

// EXERCISE CAUTION!
// This code is built for single-core MCUs. with built-in concurrency to handle multiple clients at the "same" time.
// It is NOT thread-safe!
class mfs_server {
    char* path_buffer;
    char* data_buffer;

    unsigned int path_bsize;
    unsigned int data_bsize;

    client_handlers_t* clients;
    unsigned long long clients_len;

    mfs_file_t* files;
    unsigned int files_bsize;

    write_cb client_writer;
    read_cb client_reader;
    close_cb client_killer;
    available_cb client_available;
    accept_cb accept_client;
    get_time_cb millis;


    // Helper function to populate header buffers. WILL RESULT WITH BUFFER OVERFLOW IF THE BUFFER IS SMALLER THAN 9 ELEMENTS!
    void fill_headers(char* buffer, mfs_message_t msg) {
        buffer[0] = msg.psize & 0xFF; // extract LSB
        buffer[1] = (msg.psize >> 8) & 0xFF; // second byte, and so on.
        buffer[2] = (msg.psize >> 16) & 0xFF;
        buffer[3] = (msg.psize >> 24) & 0xFF;

        buffer[4] = msg.dsize & 0xFF; // extract LSB
        buffer[5] = (msg.dsize >> 8) & 0xFF; // second byte, and so on.
        buffer[6] = (msg.dsize >> 16) & 0xFF;
        buffer[7] = (msg.dsize >> 24) & 0xFF;

        buffer[8] = msg.op;
    }

    // Helper function to read headers from buffer. WILL RESULT IN BUFFER OVERFLOW IF THE BUFFER IS SMALLER THAN 9 ELEMENTS!
    void read_headers(char* buffer, mfs_message_t* msgptr) {
        msgptr->psize = 0;
        msgptr->dsize = 0;

        // First, psize.
        msgptr->psize |= (unsigned int)buffer[0];
        msgptr->psize |= ((unsigned int)buffer[1] << 8);
        msgptr->psize |= ((unsigned int)buffer[2] << 16);
        msgptr->psize |= ((unsigned int)buffer[3] << 24);
        // then dsize.
        msgptr->dsize |= (unsigned int)buffer[4];
        msgptr->dsize |= ((unsigned int)buffer[5] << 8);
        msgptr->dsize |= ((unsigned int)buffer[6] << 16);
        msgptr->dsize |= ((unsigned int)buffer[7] << 24);

        msgptr->op = buffer[8];
    }

    // Memory compare. Checks if two buffers in memory have the same data.
    // return 1 if the data differs, and 0 when the data is the same.
    int memcmp(char* buf1, char* buf2, unsigned int buf1_size, unsigned int buf2_size) {
        // First off, check data size. If they are not equal, the data obviously isn't the same either.
         if (buf1_size != buf2_size) return 1;

         // Now, we loop to check if the data differs or not
         for (unsigned int i = 0; i < buf1_size; i++) {
             if (buf1[i] != buf2[i]) return 1;
        }
        return 0;
    }
    // String Lenght.
    // Returns the lenght of the C-string in buffer buf, returns 0 if there is no string.
    // NOTE: Returns 0 even if the string is empty. Returned string lenght does NOT include the null terminator.
    unsigned int strlen(char* buf, unsigned int buf_size) {
        unsigned int len = 0;
        for (; len < buf_size; len++) {
            if (buf[len] == '\0') break;
        }
        if (len == buf_size) return 0;
        return len;
    }

    // Copies n bytes of memory from src to dest at offset bytes.
    void memcpy(unsigned int n, char* src, char* dest, unsigned int offset) {
        for (unsigned int i = 0; i < n; i++) {
            dest[i + offset] = src[i];
        }
    }

    // Gets the index of file at path.
    // Returns the index, returns -1 if the file isn't found.
    // psize should be lenght of the string inside the path array. (Its not a C-string, just specifices how long the string is without a terminator)
    long long get_file_index(char* path, unsigned int psize) {
        for (unsigned int i = 0; i < psize; i++) {
            if (path[i] == '\0') return -1; // Illegal character.
        }

        for (unsigned int i = 0; i < this->files_bsize; i++) {
            if (this->memcmp(path, this->files[i].path, psize, this->strlen(this->files[i].path, this->files[i].path_size))) continue;
            return i;
        }
        return -1;

    }

    // closes client and removes them from the concurrent client list.
    // returns 1 on error (the only possible error condition is that the client does not exist.)
    // returns 0 on success
    int drop_client(client_t client) {
        if (client == 0) return 0; // empty client descriptor
        client_handlers_t* clients = this->clients;
        char is_client_found = 0;
        for (unsigned long long i = 0; i < this->clients_len; i++) {
            if (client == clients[i].client) {
                this->client_killer(clients[i].client);
                clients[i].client = 0;
                return 0;
            }
        }
        // If we're here the client wasn't found.
        return 1;
    }

    // checks if the file at index is empty.
    // returns 1 if it is empty, 0 if its filled.
    int is_file_empty(unsigned int index) {
        int result = 0;
        if (this->files[index].path_size == 0 && this->files[index].path == 0 && this->files[index].reader_f == 0 && this->files[index].writer_f == 0) return 1;
        return 0;
    }

    // Sends MFS message, returns -1 on error, 0 on success.
    // DROPS CLIENTS IF WRITING FAILS!
    int send_mfs_message(mfs_message_t msg, client_t client) {
        // First, build up first 9 byte buffer to send for headers.
        char buffer[9];
        this->fill_headers(buffer, msg);
        // and then write
        if (this->client_writer(client, buffer, 9) != 9) {
            // So, we can't write headers to client, in this case we are toast! drop client.
            this->drop_client(client);
            return -1;
        }
        // now write path and data.
        if (this->client_writer(client, msg.path, msg.psize) != msg.psize) {
            // Failure, drop client.
            this->drop_client(client);
            return -1;
        }

        if (this->client_writer(client, msg.data, msg.dsize) != msg.dsize) {
            // Failure, drop client.
            this->drop_client(client);
            return -1;
        }
        return 0;
    }

    // Sends corresponding error message to client of msg.
    // Inherits dropping clients from send_mfs_message() on error. Returns -1 on error, 0 on success.
    int send_mfs_error(mfs_message_t msg, client_t client, unsigned short error_code) {
        // as a reminder to future me, if what the function gets is not a pointer, it has a local copy of the arguement.
        msg.op = RESPONSE_OF(OP_ERROR);
        msg.dsize = 2;
        msg.data = this->data_buffer;

        this->data_buffer[0] = 0;
        this->data_buffer[1] = 0;

        this->data_buffer[0] = (error_code & 0xFF); // lsb of error code
        this->data_buffer[1] = (error_code >> 8) & 0xFF; // second byte

        // path is echoed back
        return this->send_mfs_message(msg, client);
    }

    // Reads MFS message, sends error to client if the data and/or psize is larger than the buffers.
    // On error, returns a MFS message struct with all (except op) as zero, and the pointers as NULL.
    // Can drop clients if erroring out errors out, Or if the client's request exceeds hard limits.
    mfs_message_t read_mfs_message(client_t client) {
        char buffer[9];
        mfs_message_t empty_error_msg = {.psize = 0, .dsize = 0, .op = RESPONSE_OF(OP_ERROR), .path = 0, .data = 0};
        mfs_message_t result;
        if (this->client_reader(client, buffer, 9) != 9) {
            // Can't read headers.
            this->send_mfs_error(empty_error_msg, client, 3);
            return empty_error_msg;
        }
        this->read_headers(buffer, &result);

        if (result.psize > this->hard_limit || result.dsize > this->hard_limit) {
            this->drop_client(client);
            return empty_error_msg;
        }

        // ===================CONSUME DATA IF DATA OR PATH SIZE IS TOO LARGE====================
        // Now, check if dsize or psize exceed limits. If so, consume the data and send error to client.
        if (result.psize > this->path_bsize || result.dsize > this->data_bsize) {
            // Consume the path.
            unsigned int chunk_size = 0;

            for (unsigned int processed_data = 0; processed_data < result.psize;) {
                if ((result.psize - processed_data) > this->path_bsize) chunk_size = this->path_bsize;
                else chunk_size = (result.psize - processed_data);

                // Read chunk
                if (this->client_reader(client, this->path_buffer, chunk_size) != chunk_size) {
                    // So, this is a really bad situation. We wanna consume data, yet we can't.
                    // Drop client.
                    this->drop_client(client);
                }

                processed_data += chunk_size;
                break;
            }

            chunk_size = 0;
            // Same for data
            for (unsigned int processed_data = 0; processed_data < result.dsize;) {
                if ((result.dsize - processed_data) > this->data_bsize) chunk_size = this->data_bsize;
                else chunk_size = (result.dsize - processed_data);

                // Read chunk
                if (this->client_reader(client, this->data_buffer, chunk_size) != chunk_size) {
                    // So, this is a really bad situation. We wanna consume data, yet we can't.
                    // Drop client.
                    this->drop_client(client);
                    break;
                }

                processed_data += chunk_size;
            }
            this->send_mfs_error(empty_error_msg, client, 001);
            return empty_error_msg;
        }
        //========================================================================================

        // Here, we are ABSOLUTELY sure the data and path can fit into our buffers.
        // Read path first (as defined by specification) and then data.
        if (this->client_reader(client, this->path_buffer, result.psize) != result.psize) {
            this->send_mfs_error(empty_error_msg, client, 001);
            return empty_error_msg;
        }
        if (this->client_reader(client, this->data_buffer, result.dsize) != result.dsize) {
            this->send_mfs_error(empty_error_msg, client, 001);
            return empty_error_msg;
        }
        // Finally, we can return the result. and change the pointers on the result struct.
        result.data = this->data_buffer;
        result.path = this->path_buffer;
        return result;
    }

    // Sends the list of files to the client.
    // Silently drops clients if sending the paths fail for some reason, as it breaks the protocol's synchronisation.
    void list_files(client_t client) {
        // Since we are on a constrained MCU, We're gonna have to manually write out the paths of the files. (Individually write them, instead of one big malloc-buffer write.)
        //  so we just copy-paste some code from the send_mfs_message function.
        // First, we will need a total size lenght of the total file paths.
        unsigned int total_size = 0;
        for (unsigned int i = 0; i < this->files_bsize; i++) {
            total_size += this->strlen(this->files[i].path, this->files[i].path_size); // No string means 0 output, so this addition is safe.
            total_size += 1; // nterminator
        }
        if (total_size <= this->data_bsize) {
            // So, the data can fit into the data buffer, we use this to directly call send_mfs_message do the job for us.
            unsigned int data_processed = 0;
            for (unsigned int i = 0; i < this->files_bsize; i++) {
                unsigned int str_len = this->strlen(this->files[i].path, this->files[i].path_size);
                if (str_len == 0) continue;
                // First copy over the path
                this->memcpy(str_len, this->files[i].path, this->data_buffer, data_processed);
                data_processed += str_len;
                this->data_buffer[data_processed] = '\0';
                data_processed++;
            }
            // Now, the buffer is ready.
            mfs_message_t msg;
            msg.dsize = data_processed;
            msg.psize = 0;
            msg.op = RESPONSE_OF(OP_LS);
            msg.data = this->data_buffer;
            msg.path = this->path_buffer;

            this->send_mfs_message(msg, client);
            return;
        }

        // Now, this is the hard part. We cannot rely on send_mfs_message to do the job for us, we simply have to do it ourselves.
        mfs_message_t msg;
        msg.op = RESPONSE_OF(OP_LS);
        msg.psize = 0;
        msg.dsize = total_size;

        // First, build up first 9 byte buffer to send for headers.

        char buffer[9];
        this->fill_headers(buffer, msg);
        // and then write
        if (this->client_writer(client, buffer, 9) != 9) {
            // So, we can't write headers to client, in this case we are toast! drop client.
            this->drop_client(client);
            return;
        }
        // Now we loop over the files writing the paths and newlines.
        char terminator = '\0';
        for (unsigned int i = 0; i < this->files_bsize; i++) {
            unsigned int str_len = this->strlen(this->files[i].path, this->files[i].path_size);
            if (str_len == 0) continue;
            if (this->client_writer(client, this->files[i].path, str_len) != str_len) {
                // Failure, so drop client.
                this->drop_client(client);
                return;
            }
            if (this->client_writer(client, &terminator, 1) != 1) {
                // Failure, so drop client.
                this->drop_client(client);
                return;
            }
        }
    }

public:
    unsigned int timer_ms = 20000; // Client timeout.
    unsigned int hard_limit = 10000; // This is a hard limit that defines the maximum amount of bytes before a client is dropped. It protects against DoS attacks.

    // Finally, the quintessential loop that serves the clients of MFS.
    void serve_clients() {
        mfs_message_t noop_response;
        noop_response.data = 0;
        noop_response.path = 0;
        noop_response.dsize = 0;
        noop_response.psize = 0;
        noop_response.op = RESPONSE_OF(OP_NOOP);
        for (unsigned int i = 0; i < this->clients_len; i++) {
            if (this->clients[i].client == 0) continue;

            if (this->clients[i].timer_end <= this->millis()) {
                // Client has expired.
                this->send_mfs_error(noop_response, this->clients[i].client, 3000);
                this->drop_client(this->clients[i].client);
                continue;
            }

            if (client_available(this->clients[i].client) >= 9) {
                mfs_message_t client_request = this->read_mfs_message(this->clients[i].client);
                if (client_request.data == 0 && client_request.path == 0 && client_request.dsize == 0 && client_request.psize == 0) {
                    // Reading client's request failed. We are most likely de-synchronised, so we drop it.
                    this->drop_client(this->clients[i].client);
                    continue;
                }
                // update client's timeout before i forget to write it
                this->clients[i].timer_end = this->millis() + this->timer_ms;

                // Read MFS message does the hard-part for us, now we just check if the path exists and redirect to its file and function.
                long long file_index = this->get_file_index(client_request.path, strlen(client_request.path, client_request.psize));
                if (file_index == -1) {
                    // File does not exist.
                    if (client_request.op == OP_LS | client_request.op == OP_NOOP) goto discard_file_nonexistent;
                    this->send_mfs_error(client_request, this->clients[i].client, 1000);
                    continue;
                }
                discard_file_nonexistent:



                // now, we parse the opcode.
                switch (client_request.op) {
                    case OP_ERROR:
                        // The client should not send this, so we treat it as a no-op.
                        this->send_mfs_message(noop_response, this->clients[i].client);
                        break;

                    case OP_LS:
                        this->list_files(this->clients[i].client);
                        break;

                    case OP_NOOP:
                        this->send_mfs_message(noop_response, this->clients[i].client);
                        break;

                    case OP_READ:
                        // Call file's callback.
                        this->send_mfs_message(this->files[file_index].reader_f(client_request), this->clients[i].client);
                        break;

                    case OP_WRITE:
                        this->send_mfs_message(this->files[file_index].writer_f(client_request), this->clients[i].client);
                        break;

                    default:
                        if (client_request.op < MFS_RESERVED_OP_RANGE) {
                            // treat as no-op
                            this->send_mfs_message(noop_response, this->clients[i].client);
                        } else {
                            // Illegal op.
                            this->send_mfs_error(client_request, this->clients[i].client, 3003);
                        }
                        break;

                }



            }
        }

    }

    /* TODO
       Loop to accept new clients +
       function to register new files +
       function to de-register files +
       Server object constuctor
    */

    // Loops over client list, accepts new clients into the buffer.
    void accept_clients() {
        for (unsigned long long i = 0; i < this->clients_len; i++) {
            if (this->clients[i].client == 0) this->clients[i].client = this->accept_client();
        }
    }

    // Registers a new file with the server object.
    // Returns 0 on success, 1 on error.
    int register_file(mfs_file_t* newfile) {
        // First, check if the path is already used.
        if (this->get_file_index(newfile->path, this->strlen(newfile->path, newfile->path_size)) != -1) return 1; // File exists, so we cannot add this file
        // Now, find an empty slot to put it in.
        unsigned int empty_slot_index = 0;
        int found_empty_slot = 0;
        for (unsigned int i = 0; i < this->files_bsize; i++) {
            if (this->files[i].path == 0 && this->files[i].path_size == 0) {
                empty_slot_index = i;
                found_empty_slot = 1;
                break;
            }
        }
        if (found_empty_slot == 0) return 1; // No empty slot.

        this->files[empty_slot_index].path = newfile->path;
        this->files[empty_slot_index].path_size = newfile->path_size;
        this->files[empty_slot_index].reader_f = newfile->reader_f;
        this->files[empty_slot_index].writer_f = newfile->writer_f;

        return 0;
    }

    // De-registers file from the files array.
    // Returns 0 on success, 1 on error.
    int unregister_file(char* path, unsigned int path_size) {
        // Check if file exists
        unsigned int file_index = this->get_file_index(path, this->strlen(path, path_size));
        if ( file_index == -1) return 1; // File does not exist.

        this->files[file_index].path = 0;
        this->files[file_index].path_size = 0;
        this->files[file_index].reader_f = 0;
        this->files[file_index].writer_f = 0;
        return 0;
    }

    // Finally; The constuctor. The beginning, of it all.
    mfs_server(read_cb readerf, write_cb writerf, accept_cb acceptf, close_cb closef, get_time_cb timef, available_cb availf, char* dbuf, unsigned int dbuf_size, char* pbuf, unsigned int pbuf_size, client_handlers_t* cbuf, unsigned int cbuf_size, mfs_file_t* fbuf, unsigned int fbuf_size) {
        this->accept_client = acceptf;
        this->client_available = availf;
        this->client_killer = closef;
        this->client_reader = readerf;
        this->client_writer = writerf;
        this->millis = timef;
        this->data_buffer = dbuf;
        this->data_bsize = dbuf_size;
        this->path_buffer = pbuf;
        this->path_bsize = pbuf_size;
        this->clients = cbuf;
        this->clients_len = cbuf_size;
        this->files = fbuf;
        this->files_bsize = fbuf_size;
    }
};
