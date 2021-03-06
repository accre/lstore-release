/*
   Copyright 2016 Vanderbilt University

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

//***********************************************************************
// Default cred setup
//***********************************************************************

#define _log_module_index 185

#include <stdlib.h>
#include <string.h>
#include <tbx/assert_result.h>
#include <tbx/type_malloc.h>

#include "authn.h"

//***********************************************************************

char *cdef_get_type(lio_creds_t *c)
{
    return("DEFAULT");
}

//***********************************************************************

void *cdef_get_type_field(lio_creds_t *c, int index, int *len)
{
    *len = 0;
    return(NULL);
}

//***********************************************************************

char *cdef_get_id(lio_creds_t *c)
{
    return(c->id);
}

//***********************************************************************

void cdef_set_id(lio_creds_t *c, char *id)
{
    c->id = strdup(id);
    return;
}

//***********************************************************************

void cdef_set_private_handle(lio_creds_t *c, void *handle, void (*destroy)(void *))
{
    c->handle = handle;
    c->handle_destroy = destroy;
    return;
}

//***********************************************************************

void *cdef_get_private_handle(lio_creds_t *c)
{
    return(c->handle);
}


//***********************************************************************

void cdef_destroy(lio_creds_t *c)
{
    if (c->handle_destroy != NULL) c->handle_destroy(c);
    if (c->id != NULL) free(c->id);
    free(c);
}

//***********************************************************************

lio_creds_t *cred_default_create()
{
    lio_creds_t *c;
    tbx_type_malloc_clear(c, lio_creds_t, 1);

    c->get_type = cdef_get_type;
    c->get_type_field = cdef_get_type_field;
    c->get_id = cdef_get_id;
    c->set_id = cdef_set_id;
    c->set_private_handle = cdef_set_private_handle;
    c->get_private_handle = cdef_get_private_handle;
    c->destroy = cdef_destroy;

    return(c);
}

