#include <stdio.h>
#include <czmq.h>

int main(void)
{
    zsys_dir_create("keys");

    // server certificate
    zcert_t *server_cert = zcert_new();
    zcert_save(server_cert, "keys/router.cert");

    // client certificates
    zcert_t *client_cert = zcert_new();
    zcert_save(client_cert, "keys/user1.cert");

    zcert_t *client_cert2 = zcert_new();
    zcert_save(client_cert2, "keys/user2.cert");

    zcert_destroy(&server_cert);
    zcert_destroy(&client_cert);
    zcert_destroy(&client_cert2);

    printf("Certificates generated.\n");
    return 0;
}


