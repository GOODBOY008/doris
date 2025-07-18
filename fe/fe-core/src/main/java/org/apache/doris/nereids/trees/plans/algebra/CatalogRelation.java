// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.doris.nereids.trees.plans.algebra;

import org.apache.doris.catalog.DatabaseIf;
import org.apache.doris.catalog.TableIf;
import org.apache.doris.nereids.exceptions.AnalysisException;
import org.apache.doris.nereids.trees.expressions.Slot;

import com.google.common.collect.ImmutableList;

import java.util.Collection;
import java.util.List;

/** CatalogRelation */
public interface CatalogRelation extends Relation {

    TableIf getTable();

    DatabaseIf getDatabase() throws AnalysisException;

    List<String> getQualifier();

    default CatalogRelation withOperativeSlots(Collection<Slot> operativeSlots) {
        return this;
    }

    default List<Slot> getOperativeSlots() {
        return ImmutableList.of();
    }
}
