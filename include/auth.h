#ifndef AUTH_H
#define AUTH_H

#define $authenticated
#define $public
#define $role(name)
#define $user_id() auth_user_id()
#define $require_owner(id) auth_require_owner(id)
#define $require_role(role) auth_require_role(role)

int auth_init(void);
void auth_bind(const void *request);
void auth_reset(void);
int auth_gate(int public_route, const char *role, int required);
int auth_user_id(void);
void auth_require_owner(int id);
void auth_require_role(const char *role);

#endif
