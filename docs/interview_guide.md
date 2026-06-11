# Mini-OSS Interview Guide

这份文档用于把 Mini-OSS 讲成一个正式 C++ 后端/软件开发岗位的工程项目。重点不是背功能清单，而是讲清楚：遇到什么问题、为什么这样设计、代码如何落地、怎么验证它稳定。

## 项目定位

Mini-OSS 是一个基于 Linux C++17 实现的单机对象存储服务，提供对象上传、下载、列表、删除、Range 下载、SHA-256 去重、秒传、Token 鉴权、SQLite 元数据持久化、Redis 元数据缓存、异步日志、运行指标、资源保护、CI、Sanitizer 和 ab/wrk 压测报告。

面试时不要把它说成完整分布式对象存储。更准确的说法是：

> 我做的是一个单机版对象存储后端服务，重点放在 Linux C++ 网络服务端、对象存储核心链路、元数据管理、缓存、资源控制、可观测性和工程化验证上。它不是简单文件上传 demo，而是按后端服务的方式做了配置、日志、监控、测试、压测、CI 和部署。

## 一分钟介绍

可以这样讲：

> Mini-OSS 是我用 C++17 在 Linux 下实现的高并发对象存储服务。网络层使用非阻塞 socket + epoll 处理连接事件，业务处理通过线程池执行，避免阻塞事件循环。对象层支持上传、下载、Range 下载、删除、SHA-256 去重和秒传；元数据用 SQLite 持久化，并用 Redis 做 cache-aside 缓存加速对象元数据和去重索引查询。为了更接近真实后端服务，我还做了 Token 鉴权、异步日志、/metrics 指标、连接数和队列上限、请求和上传超时、大文件上传临时文件流式处理、大文件下载 EPOLLOUT + sendfile 发送。最后用 CTest、Python 冒烟测试、ASan/UBSan、GitHub Actions 和 ab/wrk 压测报告验证功能正确性、稳定性和性能表现。

这段话覆盖了大厂 C++ 后端常看的点：Linux 网络编程、多线程、存储、缓存、工程化、稳定性、验证方法。

## 面试讲解主线

如果面试官让你介绍项目，建议按这条主线讲，而不是从功能列表开始背：

```text
项目背景 -> 架构设计 -> 核心难点 -> 模块实现 -> 遇到的问题 -> 怎么优化
```

### 1. 项目背景

可以这样开场：

> 我想做一个比普通文件上传 demo 更接近后端正式岗位要求的 C++ 项目，所以选择做单机对象存储服务。这个方向能覆盖 Linux 网络编程、多线程、HTTP 协议、文件存储、元数据管理、缓存、日志、监控、测试和压测。项目目标不是做完整分布式 OSS，而是先把单机后端服务的核心链路和工程化能力做扎实。

这里要主动限定边界：单机对象存储，不夸大成分布式对象存储。

### 2. 架构设计

讲架构时抓住“一条请求怎么流动”：

> 客户端请求先进入 non-blocking socket + epoll 网络层，epoll 线程负责连接事件、读写和超时管理；完整请求提交到线程池，线程池执行 HTTP 路由、对象逻辑、SQLite、文件 IO 和 SHA-256 计算；对象元数据先查 Redis cache-aside 缓存，miss 后回源 SQLite；对象内容保存在 Linux 文件系统；日志和指标独立出来用于观测和压测验证。

这个讲法能自然带出 epoll、线程池、Redis、SQLite、文件系统、日志和 metrics。

### 3. 核心难点

重点讲 5 个难点，不要平均用力：

1. **连接并发和阻塞隔离**：epoll 管连接，线程池处理业务，避免文件 IO 和 SQLite 阻塞事件循环。
2. **大文件上传内存控制**：超过阈值后流式写临时文件，避免 body 全部进内存。
3. **大文件下载效率**：file-backed response + EPOLLOUT + sendfile，避免完整文件读入用户态内存。
4. **对象去重和删除一致性**：SHA-256 + size 去重，多条元数据可指向同一物理文件，删除时按引用数清理。
5. **缓存正确性和降级**：Redis 只做 cache-aside 加速，SQLite 是 source of truth，Redis 异常时回源并记录指标。

### 4. 每个模块怎么实现

可以按模块展开：

- **网络模块**：创建监听 socket，设置非阻塞，注册 epoll；连接状态保存在 connection state 中；读请求、写响应、扫描超时。
- **HTTP 模块**：解析请求行、Header、Content-Length 和 Range；构造 200/201/206/400/401/404/413/416/503 等响应。
- **线程池模块**：完整请求进入 worker queue；队列可配置上限，满了返回 503。
- **对象模块**：处理上传、下载、列表、删除、秒传、去重、文件名清理和引用计数。
- **元数据模块**：SQLite 保存对象 id、filename、path、size、sha256、created_at，建立去重索引和 path 索引。
- **缓存模块**：Redis 缓存 object metadata 和 sha index，miss 回源 SQLite，写入/删除时更新缓存。
- **观测模块**：异步 access/error/slow log，`/metrics` 暴露连接、状态码、上传下载、缓存和延迟指标。
- **质量模块**：CTest、smoke test、Redis smoke、CI、ASan/UBSan、ab/wrk benchmark。

### 5. 遇到的问题

这里要讲真实工程问题，面试官会更信：

- **TCP/HTTP 边界问题**：路由测试发现 `/objects/` 不应该误匹配 `/objects/{id}`，因此补了路由边界测试。
- **C++ 临时对象生命周期问题**：ASan 抓到 Redis 元数据解析中 `value.substr(...).c_str()` 的 `stack-use-after-scope`，修复为保存 `length_text` 后再传给 `strtoull`。
- **大文件内存风险**：早期请求 body 走内存 buffer，不适合大文件，因此引入临时文件流式上传。
- **慢客户端和资源耗尽**：补充连接上限、队列上限、请求超时、上传超时和大小限制。
- **性能优化不能只靠描述**：Redis 和 sendfile 做完后，刷新 benchmark，用 Redis probe 和 `/metrics` 证明优化路径真的发生。

### 6. 怎么优化

已经完成的优化：

- 用 epoll 替代阻塞 accept/read，提升连接管理能力。
- 用线程池隔离业务阻塞任务，避免事件循环被 SQLite/文件 IO/SHA-256 卡住。
- 用大文件上传临时文件流式处理控制内存。
- 用 EPOLLOUT + sendfile 优化大文件下载。
- 用 Redis cache-aside 减少热点元数据和去重索引查询对 SQLite 的压力。
- 用异步日志减少同步磁盘写入对请求路径的影响。
- 用资源保护让过载时失败快，而不是无限排队。
- 用 ab/wrk、/metrics、ASan/UBSan 和 CI 做验证闭环。

后续还能继续优化：

- HTTP keep-alive，减少频繁建连开销。
- 分片上传和断点续传，增强大文件上传能力。
- 签名 URL 或用户权限模型，替代静态 Token。
- MetadataStore 替换为 MySQL/PostgreSQL，或增加连接池。
- Prometheus metrics、火焰图、heap profiling、长时间压测。
- 后台 GC 清理异常残留临时文件和无引用对象。

## 架构主线

推荐按这条线讲，不容易乱：

```text
client/curl/ab/wrk
  -> non-blocking socket + epoll
  -> HttpServer 解析请求、管理连接、超时和鉴权
  -> ThreadPool 执行业务任务
  -> Router 分发 HTTP API
  -> ObjectStore 处理对象上传/下载/删除/去重/秒传
  -> Redis Metadata Cache
  -> SQLite MetadataStore + Linux file system
  -> async logger + /metrics + benchmark report
```

核心分层：

- 网络层：非阻塞 socket、epoll、连接生命周期、读写事件、超时扫描。
- HTTP 层：请求行、Header、Body、Content-Length、Range、状态码、响应序列化。
- 业务层：对象上传、下载、列表、删除、秒传、去重、文件名清理。
- 存储层：文件系统保存对象内容，SQLite 保存元数据。
- 缓存层：Redis cache-aside 缓存对象元数据和 SHA-256 去重索引。
- 观测层：access/error/slow log、/metrics、压测报告。
- 质量层：单测、冒烟测试、CI、Sanitizer。

## 为什么这样设计

### 1. 为什么用 epoll + 线程池？

面试说法：

> epoll 适合管理大量连接的 IO 就绪事件，但文件 IO、SQLite、SHA-256 计算可能阻塞。如果都放在 epoll 线程里，会影响其他连接的响应。所以我让 epoll 线程负责 accept/read/write/timeout，完整请求进入线程池，业务逻辑在线程池里执行。

实现落点：

- epoll 线程处理非阻塞 fd、连接读写、超时扫描。
- ThreadPool 处理路由、SQLite 操作、文件操作、SHA-256。
- eventfd 用于 worker 完成后唤醒 epoll 线程发送响应。

取舍：

- 优点：IO 事件和业务阻塞任务分离，结构清晰。
- 限制：当前一连接一请求，没有实现 keep-alive 和 HTTP pipelining。

可验证点：

- 冒烟测试覆盖并发上传、超时、队列/连接保护。
- /metrics 统计 active/peak/total connections、queue_rejections、request_timeouts。

### 2. 为什么元数据先用 SQLite？

面试说法：

> 这个项目定位是单机对象存储服务。SQLite 部署简单、测试稳定，但仍然能体现后端存储设计，比如表结构、索引、WAL、事务、持久化和去重查询。为了后续扩展，我把元数据层抽出来了，后面可以替换成 MySQL 或 PostgreSQL。

实现落点：

- SQLite 保存 object id、filename、path、size、sha256、created_at。
- `(sha256, size)` 索引用于去重和秒传。
- `path` 索引用于统计一个物理文件还有多少逻辑对象引用。

取舍：

- 优点：便于本地复现，降低部署门槛，适合单机版本。
- 限制：当前是单连接 + mutex，写并发不是最终生产形态。

可验证点：

- 冒烟测试验证服务重启后元数据仍然存在。
- 删除逻辑验证共享物理文件只有在最后一个引用删除后才真正删除。

### 3. 为什么做 SHA-256 去重和秒传？

面试说法：

> 对象存储里相同内容重复上传是常见场景。用 SHA-256 + size 作为内容标识，可以让相同文件只保存一份物理内容，多条逻辑元数据指向同一个文件。秒传则是客户端提供已知 SHA-256 和 size，服务端查到已有对象后只插入元数据，不再传文件内容。

实现落点：

- 普通上传：计算 SHA-256，查 `(sha256, size)`。
- 命中已有对象：创建 metadata alias，不重复写物理文件。
- 秒传：`POST /objects/instant` 校验 `X-Object-Sha256` 和 `X-Object-Size`。
- 删除：先删元数据，再根据 path 引用数决定是否删除物理文件。

取舍：

- SHA-256 计算有 CPU 成本，但换来数据完整性和去重能力。
- 单机版没有做跨节点内容寻址和后台 GC，这是后续扩展方向。

可验证点：

- 冒烟测试验证 duplicate upload、instant upload、source delete 后 alias 仍可下载。

### 4. 为什么大文件上传要流式落临时文件？

面试说法：

> 如果所有 HTTP body 都先读到内存，大文件上传会导致内存不可控。所以我设置了 `stream_upload_threshold_bytes`，超过阈值的 `POST /objects` 直接写入临时文件，上传完成后再交给 ObjectStore 做 SHA-256、去重和落库。

实现落点：

- 小请求走内存 buffer。
- 大请求边读 socket 边写 `storage/tmp_uploads`。
- 上传完成后 worker 计算文件 SHA-256。
- 成功后 rename 到 `storage/objects/{id}`；失败、超时、断开时清理临时文件。

取舍：

- 优点：控制内存，能处理更大的对象。
- 成本：临时文件生命周期和失败清理逻辑更复杂。

可验证点：

- 冒烟测试验证大文件超过普通 request limit 仍能上传。
- 验证临时文件成功、去重、超时后能清理。
- /metrics 统计 streamed_upload_requests 和 streamed_uploaded_bytes。

### 5. 为什么下载用 EPOLLOUT + sendfile？

面试说法：

> 大文件下载如果把文件全部读进内存再拼 HTTP 响应，会造成内存浪费，也可能让慢客户端拖住发送路径。所以我把下载响应设计成 file-backed response，业务线程只打开文件并返回 fd、offset、length，epoll 线程监听 EPOLLOUT，用 sendfile 分块发送。

实现落点：

- `ObjectStore` 根据 id 找到文件，打开 fd，构造 file response。
- Range 请求解析 start/end，返回 206 和 Content-Range。
- epoll 线程先发 header，再用 sendfile 发送文件内容。
- EAGAIN/EWOULDBLOCK 时等待下一次 EPOLLOUT。

取舍：

- 优点：避免大文件进入用户态 buffer，降低内存占用，慢客户端不会阻塞整个事件循环。
- 限制：小响应仍然是内存响应；当前没有 keep-alive。

可验证点：

- 冒烟测试验证 Range 下载返回 206、非法 Range 返回 416。
- benchmark 报告单独压测大对象下载和 Range 下载。
- /metrics 统计 streamed_downloaded_bytes。

### 6. 为什么 Redis 用 cache-aside？

面试说法：

> SQLite 是元数据的权威存储，Redis 只是加速热点元数据和 SHA-256 去重索引查询。我采用 cache-aside：读时先查 Redis，miss 或 Redis 异常就回源 SQLite，成功后回填 Redis；写时先写 SQLite，再写 Redis；删除时先删 SQLite，再删除 Redis key。

实现落点：

- `prefix:object:{id}` 缓存对象元数据。
- `prefix:sha:{sha256}:{size}` 缓存内容 hash 到 object id 的索引。
- Redis 不可用时请求仍走 SQLite，并记录 metadata_cache_errors。
- TTL 限制删除失败后的脏缓存窗口。

取舍：

- 优点：正确性依赖 SQLite，Redis 挂了也能降级。
- 成本：要处理缓存不一致、删除失败、TTL 和错误指标。

可验证点：

- Redis 冒烟测试验证 hit/miss/error 指标和 SQLite fallback。
- benchmark cache probe 主动删除一个 object key，验证第一次 GET miss + SQLite fallback + refill，第二次 GET hit。
- 最新报告里 Redis probe 结果是 `hit_delta=1, miss_delta=1, error_delta=0`。

### 7. 为什么要做资源保护？

面试说法：

> 后端服务不能只考虑正常请求，还要考虑慢客户端、恶意连接、队列堆积和超大请求。所以我做了最大连接数、线程池队列上限、请求超时、上传超时、普通请求体大小限制、上传大小限制和日志队列上限。

实现落点：

- 连接超限返回 503。
- worker queue 满返回 503。
- 慢 header/body 返回 408。
- 超大请求返回 413。
- 日志队列满时丢弃并计数，不阻塞请求线程。

取舍：

- 失败快比无限排队更可控。
- 当前策略是简单阈值保护，后续可以做令牌桶限流、按 IP 限制等。

可验证点：

- 冒烟测试覆盖 max connections、slow header、slow upload。
- /metrics 暴露 rejected_connections、queue_rejections、request_timeouts、upload_timeouts。

### 8. 为什么做异步日志和 /metrics？

面试说法：

> 工程项目需要可观测性。同步写日志会把磁盘 IO 延迟带到请求路径，所以我用后台日志线程和队列。/metrics 用内存原子计数暴露连接、请求、状态码、资源保护、上传下载、缓存和延迟指标，方便测试和压测验证。

实现落点：

- access.log：remote、method、path、status、bytes、duration。
- error.log：启动、关闭、错误、日志丢弃摘要。
- slow.log：超过 slow_request_ms 的请求。
- /metrics：JSON counters。

取舍：

- 指标是进程内计数，重启会清零。
- 后续可扩展 Prometheus 格式和持久化指标。

可验证点：

- 冒烟测试检查日志文件存在。
- 压测报告记录 `/metrics` snapshot。

### 9. 为什么做 CI、单测、Sanitizer、压测？

面试说法：

> 我不想只说功能跑通，所以加了多层验证。单测覆盖 HTTP parser、router、config、object store、Range 等基础逻辑；冒烟测试跑真实 server 流程；CI 自动构建和测试；ASan/UBSan 检查 C++ 内存安全和未定义行为；ab/wrk 生成可复现压测报告。

真实可讲的两个点：

- 单测曾发现 `/objects/` 不应该匹配 `/objects/{id}` 的路由边界问题。
- ASan 曾发现 Redis 元数据解析里 `value.substr(...).c_str()` 临时字符串生命周期导致的 `stack-use-after-scope`，修复方式是把 substring 保存到 `length_text`，保证 `strtoull` 使用时内存仍有效。

压测可讲：

- 基础链路：`GET /health`。
- 读链路：小对象下载、大对象 EPOLLOUT + sendfile 下载、Range 下载。
- 写链路：普通上传、秒传。
- 缓存链路：Redis miss/hit probe。
- 所有报告场景 failed=0，Redis probe 验证 hit/miss/error 指标符合预期。

## 关键流程讲解

### 上传流程

```text
client POST /objects
  -> epoll read headers/body
  -> small body in memory OR large body to tmp file
  -> worker thread handles ObjectStore
  -> compute SHA-256
  -> check Redis/SQLite by sha256 + size
  -> duplicate: insert metadata alias, remove temp file
  -> new object: move file into storage/objects, insert metadata
  -> backfill Redis cache
  -> return 201 Created
```

可以强调：

- 内存控制：大文件不整体进内存。
- 完整性：SHA-256 元数据。
- 去重：逻辑对象和物理文件分离。
- 异常处理：失败清理临时文件。

### 下载流程

```text
client GET /objects/{id}
  -> auth
  -> worker reads metadata from Redis/SQLite
  -> open physical file
  -> build file-backed response
  -> eventfd wakes epoll
  -> EPOLLOUT sends headers
  -> sendfile sends file body in chunks
  -> metrics/log after completion
```

可以强调：

- 不把大文件加载到内存。
- Range 支持断点/部分下载。
- fd 生命周期保证 in-flight download 稳定。

### Redis 查询流程

```text
getObject(id)
  -> Redis GET object key
  -> hit: return ObjectInfo
  -> miss/error: query SQLite
  -> SQLite hit: SETEX backfill Redis
  -> return metadata
```

可以强调：

- SQLite 是 source of truth。
- Redis 是性能优化，不影响正确性。
- `/metrics` 能证明缓存路径发生了。

### 删除流程

```text
DELETE /objects/{id}
  -> lookup metadata
  -> delete metadata row
  -> invalidate Redis object and sha index keys
  -> count remaining rows by same physical path
  -> if count == 0: remove physical file
  -> return deleted + removed_file
```

可以强调：

- 去重后不能一删 metadata 就删物理文件。
- 引用计数通过 path 查询实现。
- Redis 删除失败由 TTL 限制脏数据窗口。

## 面试官可能追问

### Q1：你的项目和普通文件上传 demo 有什么区别？

回答：

> 普通 demo 可能只实现上传下载。我这个项目按后端服务做了网络层、线程池、HTTP 协议处理、对象元数据、SHA-256 去重、秒传、大文件上传流式落盘、大文件下载 sendfile、Redis 缓存、资源保护、日志、metrics、CI、Sanitizer、冒烟测试和压测报告。它更关注服务端工程能力，而不只是功能按钮。

### Q2：为什么不用现成框架？

回答：

> 我这个项目的目标是训练 C++ 后端底层能力，所以网络层和 HTTP 层自己实现，重点理解非阻塞 IO、epoll、线程池、HTTP 解析、连接状态和资源控制。生产项目当然会评估成熟框架，但简历项目里自己实现能更好地展示基础能力。

### Q3：Redis 缓存如何保证一致性？

回答：

> 我采用 cache-aside，SQLite 是权威存储。写入时先写 SQLite，再写 Redis；删除时先删 SQLite，再删 Redis。Redis 删除失败时，TTL 会限制脏缓存时间，同时记录 metadata_cache_errors。严格强一致可以用事务/消息队列/版本号，但这个单机项目选择了更容易解释和部署的最终一致缓存方案。

### Q4：sendfile 有什么好处？

回答：

> sendfile 可以减少用户态和内核态之间的数据拷贝。下载大文件时不需要把整个文件读到用户态 buffer 再 write 到 socket。我这里把文件响应交给 epoll 的 EPOLLOUT 路径，按 chunk 调 sendfile，遇到 EAGAIN 就等下一次可写事件。

### Q5：大文件上传时服务端崩溃怎么办？

回答：

> 当前版本在正常失败、断开和超时路径会清理临时文件；服务进程异常崩溃后，tmp_uploads 里可能残留临时文件。后续可以在启动时扫描 tmp_uploads，根据文件 mtime 清理过期临时文件，或者引入后台 GC 任务。

### Q6：为什么当前没有 keep-alive？

回答：

> 我先把重点放在 epoll、线程池、对象链路、资源保护和工程化验证上，所以当前是一连接一请求，连接状态简单、容易测试。keep-alive 会引入请求复用、半包状态、响应队列和超时策略，后续可以作为网络层增强。

### Q7：SQLite 会不会成为瓶颈？

回答：

> 会，当前 SQLite 是单连接加 mutex，适合单机项目和功能验证。为了缓解热点读，我加了 Redis 元数据缓存。后续如果继续工程化，可以把 MetadataStore 抽象替换为 MySQL/PostgreSQL，或者做 SQLite 连接池、读写分离、批量写入。

### Q8：线程池队列满了怎么办？

回答：

> 我没有无限堆积请求，而是返回 503，并在 metrics 中记录 queue_rejections。这样做是为了让过载表现可控，避免排队导致整体延迟不可预期。

### Q9：你怎么证明项目稳定？

回答：

> 我用了分层验证：CTest 覆盖基础模块，Python smoke test 跑真实 server，Redis smoke test 覆盖缓存和降级，ASan/UBSan 检查 C++ 内存安全，GitHub Actions 做自动化质量门禁，ab/wrk 做可复现压测。测试过程中还发现过真实问题，比如路由边界 bug 和 ASan 抓到的临时字符串生命周期问题。

### Q10：这个项目还有哪些不足？

回答：

> 我会主动说明边界：目前是单机服务，没有分布式副本和纠删码；HTTP 不支持 keep-alive；鉴权是静态 token；SQLite 是单连接；metrics 是内存计数。后续可以继续做 keep-alive、分片上传、签名 URL、后台 GC、MySQL/PostgreSQL 元数据层、Prometheus metrics、限流和更完整的 Docker/CI 发布流程。

## 对标大厂后端能力点

| 能力点 | 项目体现 |
| --- | --- |
| Linux 网络编程 | non-blocking socket、epoll、EPOLLIN/EPOLLOUT、sendfile |
| C++ 工程能力 | C++17、RAII、CMake、模块拆分、单测、Sanitizer |
| 并发编程 | epoll + worker thread pool + async logger |
| HTTP 协议 | request parser、header、Content-Length、Range、状态码 |
| 存储设计 | 文件系统对象、SQLite 元数据、索引、WAL、引用清理 |
| 缓存设计 | Redis cache-aside、TTL、fallback、hit/miss/error metrics |
| 稳定性 | 超时、连接上限、队列上限、上传大小限制、临时文件清理 |
| 可观测性 | access/error/slow log、/metrics、benchmark report |
| 工程化验证 | CTest、smoke test、CI、ASan/UBSan、ab/wrk |
| 面试可解释性 | 每个功能都有问题背景、设计取舍和验证证据 |

## 简历表述建议

标题：

> Mini-OSS：Linux C++ 高并发对象存储服务

项目描述：

> 基于 C++17 实现单机对象存储后端服务，围绕 Linux 非阻塞网络 IO、epoll/Reactor、线程池、HTTP 协议解析、对象元数据管理、文件上传下载、缓存加速和工程化验证进行设计，支持对象上传、下载、Range 下载、SHA-256 去重、秒传、Token 鉴权、SQLite 持久化、Redis 元数据缓存、异步日志、运行指标和 ab/wrk 压测。

项目实现可以写：

1. 基于 non-blocking socket + epoll 实现事件驱动网络层，使用线程池处理路由、SQLite、文件 IO 和 SHA-256 计算，降低事件循环阻塞风险。
2. 实现 HTTP 请求解析、路由分发和响应构建，支持 `POST /objects`、`GET /objects/{id}`、Range 下载、删除、列表和 `/metrics`。
3. 使用 SQLite 保存对象元数据，基于 `(sha256, size)` 实现内容去重和秒传，删除时通过物理路径引用计数避免误删共享文件。
4. 针对大文件上传设计临时文件流式处理，超过阈值后边读 socket 边写入 `tmp_uploads`，上传完成后分块计算 SHA-256，并覆盖失败/超时清理。
5. 针对大文件下载实现 file-backed response，使用 EPOLLOUT + sendfile 分块发送文件内容，避免完整文件读入内存，并支持 Range 部分下载。
6. 引入 Redis cache-aside 元数据缓存，缓存对象信息和 SHA-256 去重索引，支持 miss 回源 SQLite、回填缓存、TTL、删除失效和异常降级。
7. 完善资源保护和可观测性，支持连接上限、线程池队列上限、请求/上传超时、上传大小限制、异步日志、慢请求日志和 `/metrics` 指标。
8. 建立工程化验证闭环，使用 CTest、Python 冒烟测试、GitHub Actions、ASan/UBSan 和 ab/wrk 压测报告验证功能正确性、内存安全和性能表现。

## 面试讲法模板

### 30 秒版

> 我做了一个 Linux C++ 单机对象存储服务，核心是 epoll + 线程池的后端服务架构。它支持对象上传下载、Range、删除、SHA-256 去重、秒传、SQLite 元数据持久化和 Redis 缓存。我还做了大文件上传流式落盘、大文件下载 EPOLLOUT + sendfile、资源保护、异步日志、metrics、CI、Sanitizer 和 ab/wrk 压测，所以它更像一个完整后端工程项目。

### 2 分钟版

> 项目网络层使用非阻塞 socket 和 epoll，epoll 线程负责连接事件和读写，完整请求交给线程池处理，避免 SQLite、文件 IO 和 SHA-256 阻塞事件循环。对象存储层把文件内容放在文件系统，元数据放在 SQLite，通过 SHA-256 + size 做去重和秒传。为了处理大文件，我做了上传流式落临时文件，避免 body 全部进内存；下载时 ObjectStore 返回文件 fd、offset、length，epoll 通过 EPOLLOUT + sendfile 发送，支持 Range。Redis 作为 cache-aside 层缓存对象元数据和去重索引，SQLite 保持权威存储，Redis 异常时可以降级。工程化方面，我做了 Token 鉴权、资源限制、异步日志、/metrics、CTest、真实 server 冒烟测试、ASan/UBSan 和 ab/wrk 压测报告。

### 5 分钟展开顺序

1. 先讲项目定位：单机对象存储服务，不是分布式系统。
2. 讲整体架构：epoll -> ThreadPool -> Router -> ObjectStore -> Redis/SQLite/FileSystem。
3. 讲核心链路：上传、下载、删除、去重、秒传。
4. 讲两个重点优化：大文件上传流式落盘，下载 EPOLLOUT + sendfile。
5. 讲缓存：Redis cache-aside，SQLite source of truth。
6. 讲稳定性：连接/队列/超时/大小限制/日志队列。
7. 讲验证：CTest、smoke、Sanitizer、CI、benchmark。
8. 主动讲限制和后续扩展：keep-alive、分片上传、签名 URL、MySQL、Prometheus、分布式。

## 不要这样说

避免夸大：

- 不要说“实现了分布式对象存储”，当前是单机。
- 不要说“生产级高可用”，当前没有副本、选主、容灾。
- 不要只说“用了 Redis”，要说 cache-aside、fallback、TTL、metrics。
- 不要只说“用了 epoll”，要说 epoll 负责什么、线程池负责什么。
- 不要只报 QPS 数字，要说明这是 WSL loopback，本地压测用于回归比较，不代表生产容量。

推荐表达：

- “单机对象存储服务”
- “面向 C++ 后端基础能力和工程化验证”
- “通过 ab/wrk 和 /metrics 证明优化路径生效”
- “当前边界清楚，后续可扩展到 keep-alive、分片上传、签名 URL、外部数据库和分布式存储”

