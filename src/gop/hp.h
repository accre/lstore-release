/* Copyright 2016 Vanderbilt University

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

/** \file
* Autogenerated public API
*/

#ifndef ACCRE_GOP_HP_PRIVATE_H_INCLUDED
#define ACCRE_GOP_HP_PRIVATE_H_INCLUDED

#include <apr_time.h>
#include <gop/visibility.h>
#include <gop/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int gop_op_sync_exec_enabled(gop_op_generic_t *gop);
void gop_op_sync_exec(gop_op_generic_t *gop);
void gop_op_submit(gop_op_generic_t *gop);

#ifdef __cplusplus
}
#endif

#endif /* ^ ACCRE_GOP_HP_PRIVATE_H_INCLUDED ^ */
