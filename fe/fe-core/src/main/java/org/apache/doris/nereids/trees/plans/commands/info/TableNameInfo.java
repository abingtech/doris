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
// This file is copied from
// https://github.com/apache/impala/blob/branch-2.9.0/fe/src/main/java/org/apache/impala/TableName.java
// and modified by Doris

package org.apache.doris.nereids.trees.plans.commands.info;

import org.apache.doris.analysis.TableName;
import org.apache.doris.catalog.Env;
import org.apache.doris.datasource.InternalCatalog;
import org.apache.doris.nereids.exceptions.AnalysisException;
import org.apache.doris.qe.ConnectContext;

import com.google.common.base.Preconditions;
import com.google.common.base.Strings;
import com.google.gson.annotations.SerializedName;

import java.util.List;
import java.util.Objects;
import java.util.stream.Stream;

/**
 * table name info
 */
public class TableNameInfo {
    @SerializedName(value = "c")
    private String ctl;
    @SerializedName(value = "t")
    private String tbl;
    @SerializedName(value = "d")
    private String db;

    public TableNameInfo() {

    }

    /**
     * TableNameInfo
     * @param parts like [ctl1,db1,tbl1] or [db1,tbl1] or [tbl1]
     */
    public TableNameInfo(List<String> parts) {
        Objects.requireNonNull(parts, "require parts object");
        int size = parts.size();
        Preconditions.checkArgument(size > 0, "table name can't be empty");
        tbl = parts.get(size - 1);
        if (Env.isStoredTableNamesLowerCase() && !Strings.isNullOrEmpty(tbl)) {
            tbl = tbl.toLowerCase();
        }
        if (size >= 2) {
            db = parts.get(size - 2);
        }
        if (size >= 3) {
            ctl = parts.get(size - 3);
        }
    }

    /**
     * TableNameInfo
     * @param db dbName
     * @param tbl tblName
     */
    public TableNameInfo(String db, String tbl) {
        Objects.requireNonNull(tbl, "require tbl object");
        Objects.requireNonNull(db, "require db object");
        this.ctl = InternalCatalog.INTERNAL_CATALOG_NAME;
        this.tbl = tbl;
        if (Env.isStoredTableNamesLowerCase()) {
            this.tbl = tbl.toLowerCase();
        }
        this.db = db;
    }

    /**
     * TableNameInfo
     * @param ctl catalogName
     * @param db dbName
     * @param tbl tblName
     */
    public TableNameInfo(String ctl, String db, String tbl) {
        Objects.requireNonNull(tbl, "require tbl object");
        this.ctl = ctl;
        this.tbl = tbl;
        if (Env.isStoredTableNamesLowerCase()) {
            this.tbl = tbl.toLowerCase();
        }
        this.db = db;
    }

    /**
     * analyze tableNameInfo
     * @param ctx ctx
     */
    public void analyze(ConnectContext ctx) {
        if (Strings.isNullOrEmpty(ctl)) {
            ctl = ctx.getDefaultCatalog();
            if (Strings.isNullOrEmpty(ctl)) {
                ctl = InternalCatalog.INTERNAL_CATALOG_NAME;
            }
        }
        if (Strings.isNullOrEmpty(db)) {
            db = ctx.getDatabase();
            if (Strings.isNullOrEmpty(db)) {
                throw new AnalysisException("No database selected");
            }
        }

        if (Strings.isNullOrEmpty(tbl)) {
            throw new AnalysisException("Table name is null");
        }
    }

    /**
     * Returns true if this name has a non-empty catalog and a non-empty database field
     * and a non-empty table name.
     */
    public boolean isFullyQualified() {
        return Stream.of(ctl, db, tbl).noneMatch(Strings::isNullOrEmpty);
    }

    /**
     * get catalog name
     * @return ctlName
     */
    public String getCtl() {
        return ctl;
    }

    /**
     * get db name
     * @return dbName
     */
    public String getDb() {
        return db;
    }

    /**
     * set a new database name
     * @param db new database name
     */
    public void setDb(String db) {
        this.db = db;
    }

    /**
     * set a new ctl name
     * @param ctl new database name
     */
    public void setCtl(String ctl) {
        this.ctl = ctl;
    }

    /**
     * get table name
     * @return tableName
     */
    public String getTbl() {
        return tbl;
    }

    /**
     * transferToTableName
     * @return TableName
     */
    public TableName transferToTableName() {
        return new TableName(ctl, db, tbl);
    }

    @Override
    public String toString() {
        StringBuilder stringBuilder = new StringBuilder();
        if (ctl != null && !ctl.equals(InternalCatalog.INTERNAL_CATALOG_NAME)) {
            stringBuilder.append(ctl).append(".");
        }
        if (db != null) {
            stringBuilder.append(db).append(".");
        }
        stringBuilder.append(tbl);
        return stringBuilder.toString();
    }

    /**
     * equals
     */
    @Override
    public boolean equals(Object o) {
        if (this == o) {
            return true;
        }
        if (o == null || getClass() != o.getClass()) {
            return false;
        }
        TableNameInfo that = (TableNameInfo) o;
        return tbl.equals(that.tbl) && db.equals(that.db) && ctl.equals(that.ctl);
    }

    /**
     * hashCode
     */
    @Override
    public int hashCode() {
        return Objects.hash(tbl, db, ctl);
    }

    /**
     * toSql
     */
    public String toSql() {
        StringBuilder stringBuilder = new StringBuilder();
        if (ctl != null && !ctl.equals(InternalCatalog.INTERNAL_CATALOG_NAME)) {
            stringBuilder.append("`").append(ctl).append("`.");
        }
        if (db != null) {
            stringBuilder.append("`").append(db).append("`.");
        }
        stringBuilder.append("`").append(tbl).append("`");
        return stringBuilder.toString();
    }

    /**
     * toFullyQualified
     */
    public String toFullyQualified() {
        StringBuilder stringBuilder = new StringBuilder();
        if (ctl != null) {
            stringBuilder.append("`").append(ctl).append("`.");
        }
        if (db != null) {
            stringBuilder.append("`").append(db).append("`.");
        }
        stringBuilder.append("`").append(tbl).append("`");
        return stringBuilder.toString();
    }
}
