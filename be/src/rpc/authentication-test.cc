// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "testutil/gtest-util.h"
#include "common/init.h"
#include "common/logging.h"
#include "rpc/authentication.h"
#include "rpc/thrift-server.h"
#include "util/network-util.h"
#include "util/thread.h"

DECLARE_bool(enable_ldap_auth);
DECLARE_string(ldap_uri);
DECLARE_string(keytab_file);
DECLARE_string(principal);
DECLARE_string(ssl_client_ca_certificate);
DECLARE_string(ssl_server_certificate);

// These are here so that we can grab them early in main() - the kerberos
// init can clobber KRB5_KTNAME in PrincipalSubstitution.
static const char *env_keytab = NULL;
static const char *env_princ = NULL;

#include "common/names.h"

namespace impala {

TEST(Auth, PrincipalSubstitution) {
  string hostname;
  ASSERT_OK(GetHostname(&hostname));
  SaslAuthProvider sa(false);  // false means it's external
  ASSERT_OK(sa.InitKerberos("service_name/_HOST@some.realm", "/etc/hosts"));
  ASSERT_OK(sa.Start());
  ASSERT_EQ(string::npos, sa.principal().find("_HOST"));
  ASSERT_NE(string::npos, sa.principal().find(hostname));
  ASSERT_EQ("service_name", sa.service_name());
  ASSERT_EQ(hostname, sa.hostname());
  ASSERT_EQ("some.realm", sa.realm());
}

TEST(Auth, ValidAuthProviders) {
  ASSERT_OK(AuthManager::GetInstance()->Init());
  ASSERT_TRUE(AuthManager::GetInstance()->GetExternalAuthProvider() != NULL);
  ASSERT_TRUE(AuthManager::GetInstance()->GetInternalAuthProvider() != NULL);
}

// Set up ldap flags and ensure we make the appropriate auth providers
TEST(Auth, LdapAuth) {
  AuthProvider* ap = NULL;
  SaslAuthProvider* sa = NULL;

  FLAGS_enable_ldap_auth = true;
  FLAGS_ldap_uri = "ldaps://bogus.com";

  // Initialization based on above "command line" args
  ASSERT_OK(AuthManager::GetInstance()->Init());

  // External auth provider is sasl, ldap, but not kerberos
  ap = AuthManager::GetInstance()->GetExternalAuthProvider();
  ASSERT_TRUE(ap->is_sasl());
  sa = dynamic_cast<SaslAuthProvider*>(ap);
  ASSERT_TRUE(sa->has_ldap());
  ASSERT_EQ("", sa->principal());

  // Internal auth provider isn't sasl.
  ap = AuthManager::GetInstance()->GetInternalAuthProvider();
  ASSERT_FALSE(ap->is_sasl());
}

// Set up ldap and kerberos flags and ensure we make the appropriate auth providers
TEST(Auth, LdapKerbAuth) {
  AuthProvider* ap = NULL;
  SaslAuthProvider* sa = NULL;

  if ((env_keytab == NULL) || (env_princ == NULL)) {
    return;     // In a non-kerberized environment
  }
  FLAGS_keytab_file = env_keytab;
  FLAGS_principal = env_princ;
  FLAGS_enable_ldap_auth = true;
  FLAGS_ldap_uri = "ldaps://bogus.com";

  // Initialization based on above "command line" args
  ASSERT_OK(AuthManager::GetInstance()->Init());

  // External auth provider is sasl, ldap, and kerberos
  ap = AuthManager::GetInstance()->GetExternalAuthProvider();
  ASSERT_TRUE(ap->is_sasl());
  sa = dynamic_cast<SaslAuthProvider*>(ap);
  ASSERT_TRUE(sa->has_ldap());
  ASSERT_EQ(FLAGS_principal, sa->principal());

  // Internal auth provider is sasl and kerberos
  ap = AuthManager::GetInstance()->GetInternalAuthProvider();
  ASSERT_TRUE(ap->is_sasl());
  sa = dynamic_cast<SaslAuthProvider*>(ap);
  ASSERT_FALSE(sa->has_ldap());
  ASSERT_EQ(FLAGS_principal, sa->principal());
}

// Test for IMPALA-2598: SSL and Kerberos do not mix on server<->server connections.
// Tests that Impala will successfully start if so configured.
TEST(Auth, KerbAndSslEnabled) {
  string hostname;
  ASSERT_OK(GetHostname(&hostname));
  FLAGS_ssl_client_ca_certificate = "some_path";
  FLAGS_ssl_server_certificate = "some_path";
  ASSERT_TRUE(EnableInternalSslConnections());
  SaslAuthProvider sa_internal(true);
  ASSERT_OK(
      sa_internal.InitKerberos("service_name/_HOST@some.realm", "/etc/hosts"));
  SaslAuthProvider sa_external(false);
  ASSERT_OK(
      sa_external.InitKerberos("service_name/_HOST@some.realm", "/etc/hosts"));
}

}

int main(int argc, char** argv) {
  impala::InitCommonRuntime(argc, argv, true, impala::TestInfo::BE_TEST);
  ::testing::InitGoogleTest(&argc, argv);

  env_keytab = getenv("KRB5_KTNAME");
  env_princ = getenv("MINIKDC_PRINC_IMPALA");

  return RUN_ALL_TESTS();
}
