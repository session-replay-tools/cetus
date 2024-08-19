# Cetus

##  Introduction

Cetus is middleware developed in C for the relational database MySQL, primarily offering comprehensive database access proxy functionality. Cetus is designed to be largely compatible with MySQL connections, allowing applications to access the database through Cetus with minimal changes, thus achieving horizontal scaling and high availability at the database layer.

## Version Selection

For production environments, it is recommended to use the latest [Release version](https://github.com/cetus-tools/cetus/releases).


## Key Features

Cetus is available in two versions: read-write splitting and sharding (where sharding is a specific form of database splitting).

**For the read-write splitting version:**

- Multi-process, lock-free design for improved efficiency
- Supports transparent backend connection pooling
- Supports SQL read-write splitting
- Enhanced SQL routing
- Supports prepared statements
- Supports result set compression
- Supports security management
- Supports status monitoring
- Supports TCP stream processing
- Supports domain name-based backend connections
- SSL/TLS support (client-side)
- Strong read consistency support (to be implemented)

**For the database sharding version:**
- Multi-process, lock-free design for improved efficiency
- Supports transparent backend connection pooling
- Supports SQL read-write separation
- Supports data sharding
- Supports distributed transaction processing
- Supports bulk insert operations
- Supports conditional DISTINCT operations
- Enhanced SQL routing and injection
- Supports result set compression
- Features a high-performance result set merging algorithm
- Supports security management
- Supports status monitoring
- Supports TCP stream processing
- Supports domain name-based backend connections
- SSL/TLS support (client-side)
- MGR support
- Strong read consistency support (to be implemented)


## Detailed Description

### Installing and Using Cetus

1. [Cetus Quick Start](./doc/cetus-quick-try.md)

2. [Cetus Installation Instructions](./doc/cetus-install.md)

3. [Cetus Read-Write Splitting Configuration File Instructions](./doc/cetus-rw-profile.md)

4. [Cetus Sharding Configuration File Instructions](./doc/cetus-shard-profile.md)

5. [Cetus Startup Configuration Options](./doc/cetus-configuration.md)

6. [Cetus Usage Constraints](./doc/cetus-constraint.md)

7. [Cetus Read-Write Splitting Edition User Guide](./doc/cetus-rw.md)

8. [Cetus Read-Write Splitting Edition Management Manual](./doc/cetus-rw-admin.md)

9. [Cetus Sharding Edition User Guide](./doc/cetus-sharding.md)

10. [Cetus Sharding Edition Management Manual](./doc/cetus-shard-admin.md)

11. [Cetus Full Log Usage Manual](./doc/cetus-sqllog-usage.md)

12. [Introduction to Cetus Routing Strategies](./doc/cetus-routing-strategy.md)

13. [Cetus partition Usage Instructions](./doc/cetus-partition-profile.md)

14. [Cetus Data Migration Tracking Tool User Manual](./dumpbinlog-tool/readme.md)

### Cetus Architecture and Design

[Cetus Architecture and Implementation](./doc/cetus-architecture.md)

### MySQL XA Transaction Issues Discovered by Cetus

[Explanation of MySQL XA Transaction Issues](./doc/mysql-xa-bug.md)

### Cetus Auxiliary

1. [Cetus XA Hang Handling Tool](./doc/cetus-xa.md)

2. [Cetus + MHA High Availability Solution](./doc/cetus-mha.md)

3. [Cetus RPM Documentation](./doc/cetus-rpm.md)

4. [Cetus Docker Image Usage](./doc/cetus-docker.md)

5. [Cetus Web-Based Graphical Management Interface](https://github.com/Lede-Inc/Cetus-GUI)

### Cetus Testing

[Cetus Testing Report](./doc/cetus-test.md)

## Note
1. Cetus runs exclusively on Linux.
2. Cetus cannot be compiled with MySQL 8.0 development.
3. Cetus supports only `mysql_native_password`.
4. For non-Chinese users, please visit visit [mysql-proxy](https://github.com/session-replay-tools/mysql-proxy).

## Bugs and Feature Requests
Have a bug or a feature request? [Please open a new issue](https://github.com/session-replay-tools/cetus/issues). Before opening any issue, please search for existing issues.

