// Copyright 2014 Cloudera Inc.
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

package com.cloudera.impala.testutil;

import com.cloudera.impala.authorization.SentryConfig;
import com.cloudera.impala.catalog.AuthorizationPolicy;
import com.cloudera.impala.catalog.CatalogException;
import com.cloudera.impala.catalog.CatalogServiceCatalog;
import com.cloudera.impala.thrift.TUniqueId;

/**
 * Test class of the Catalog Server's catalog that exposes internal state that is useful
 * for testing.
 */
public class CatalogServiceTestCatalog extends CatalogServiceCatalog {

  public CatalogServiceTestCatalog(boolean loadInBackground, int numLoadingThreads,
      SentryConfig sentryConfig, TUniqueId catalogServiceId) {
    super(loadInBackground, numLoadingThreads, sentryConfig, catalogServiceId);

    // Cache pools are typically loaded asynchronously, but as there is no fixed execution
    // order for tests, the cache pools are loaded synchronously before the tests are
    // executed.
    CachePoolReader rd = new CachePoolReader();
    rd.run();
  }

  public static CatalogServiceCatalog create() {
    return createWithAuth(null);
  }

  /**
   * Creates a catalog server that that reads authorization policy metadata from the
   * Sentry Policy Service.
   */
  public static CatalogServiceCatalog createWithAuth(SentryConfig config) {
    CatalogServiceCatalog cs =
        new CatalogServiceTestCatalog(false, 16, config, new TUniqueId());
    try {
      cs.reset();
    } catch (CatalogException e) {
      throw new IllegalStateException(e.getMessage(), e);
    }
    return cs;
  }

  @Override
  public AuthorizationPolicy getAuthPolicy() { return authPolicy_; }
}
