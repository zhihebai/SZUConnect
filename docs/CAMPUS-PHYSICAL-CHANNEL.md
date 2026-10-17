# 校园认证物理通道

## 适用场景

部分 TUN 客户端把校园域名的 DNS 查询送入 Fake-IP，或在换网后继续使用旧地址的解析连接。校园流量规则即使命中直连，未认证时仍可能无法读取校园 HTTPS 页面。单纯绕过系统 HTTP 代理不能绕过这条解析链。

## 通道选择

物理通道将请求绑定到具体校园网卡。绑定信息包括源地址、网关和 DNS，在代码中以租约（lease）表示。

macOS 与 Windows 将校园网络记忆与实际请求出口对应。存在可核验的物理候选时，固定校园认证及基础探针使用该候选的应用内通道，TUN 开关不改变网络记忆资格。未绑定的普通检测使用平台传输，常用网址按用户所选策略访问。

每条候选分别确认物理服务、活动 IPv4 默认路由（包括可绑定的 scoped 默认）及网关，两次完整快照一致后才采用。系统主候选与其他候选分别核验，不能混用。

DNS 只取该服务的 DHCP option 6 或关联的静态 DNS，不读取全局 TUN DNS，不猜测公共 DNS，也不硬编码校园服务器的解析地址。

DNS 与 TCP 套接字同时绑定物理接口和当前源 IPv4。多地址接口绑定生成网络描述时选用的首个 IPv4 地址，使请求源与所记子网对应。

持久匹配使用所选候选的网络描述，不使用连接组合或 TUN 租约作为标识。应用模型为每次操作指定唯一物理租约；客户端重置及探测不自行换网，同一传输代次使用相同的绑定信息。

连接建立前、发送前及返回后重新核验，换网或取消后拒绝旧代次结果。活动 TUN 下，物理出口不明、关联 DNS 缺失或绑定失败均停止请求，不回退到代理。无 TUN 的普通 IPv6 连接使用系统传输。

## 请求限制

DNS、网卡信息及服务器响应均需校验。校园身份由服务核验确认，DNS 回答和 Wi-Fi 名称只作网络信息使用。

- 只接受现有固定 HTTPS 校园域名、宿舍 IP 和基础探针的既定协议、端口与路径，不提供任意网址抓取。
- macOS 使用系统 libcurl；Windows 静态链接固定版本 libcurl 并使用 Schannel。认证 URL 保留域名，不改写为解析出的 IP；TLS 主机名及证书链验证始终开启。
- 禁止系统代理、netrc、自动跳转、Cookie 文件、调试日志和凭据命令行参数；凭据与 Cookie 仅在内存。
- 核验校园 HTTPS 页面成功后，才允许继续宿舍 HTTP 页面及认证流程。显式信任、网络记忆资格、单次提交、90 秒冷却、取消代次及结果不明保护均保持有效。
- 响应正文与头部大小、DNS 名称压缩和记录数量、超时、取消及 Cookie 保留均有限制。
- 认证 Cookie 与只读发现／探针隔离，所选物理连接变化时重建并取消旧通道；仅 TUN、其他网卡或主网络元数据变化时保留未变化的绑定。

应用不修改 VPN、代理、DNS 或路由，也不调用代理客户端的配置接口。

## 限制与验证

物理通道仅处理 IPv4。关联 DNS 在同一总预算内最多发送三次，首发后 250、750 毫秒重传。匹配请求的拒绝或截断回复结束该服务器的等待，其他服务器仍可返回有效答案。

DNS 响应截断，或 CNAME 链在同包内没有可用 A 记录时明确失败，不回退到全局 DNS。路由证据不完整、TUN 策略禁止绑定接口，或未认证网络没有可用 DNS 时，不能保证通道可达。

离线验证覆盖物理默认路由、拓扑变化、DNS 数据包边界、源接口绑定、传输安全、Cookie 隔离、取消、代次失效、系统 DNS 失败及认证保护。

原生只读验证需在 TUN 开启时解析并核验校园页面。掉线后的实际登录恢复须单独测试，已经登录时收到 HTTP 200 只能证明当时可访问。

## 官方资料

- [libcurl 自定义套接字](https://curl.se/libcurl/c/CURLOPT_OPENSOCKETFUNCTION.html)
- [libcurl 指定解析结果](https://curl.se/libcurl/c/CURLOPT_RESOLVE.html)
- [TLS 主机名验证](https://curl.se/libcurl/c/CURLOPT_SSL_VERIFYHOST.html)
- [TLS 证书链验证](https://curl.se/libcurl/c/CURLOPT_SSL_VERIFYPEER.html)
- [发送前回调](https://curl.se/libcurl/c/CURLOPT_PREREQFUNCTION.html)

## Windows 实现

Windows 使用 IP Helper 双快照核验接口 LUID、索引、活动源地址、子网、默认网关及关联 DNS。`GetIfEntry2` 还需确认匹配的活动硬件接口并排除 Filter、Endpoint；类型或名称不能单独证明物理网卡身份。

UDP／TCP 绑定源 IPv4 并设置 `IP_UNICAST_IF`，连接及 HTTP 发送前再次核验租约。接口首选标志、名称、其他网卡及 TUN 路由不参与所选物理租约的相等判断。普通网站按 WinHTTP 策略访问，网站代次变化只取消网站子任务。

Windows CMake 使用固定 libcurl 源码、静态运行库和 Schannel，不附带另一套 CA 文件。TLS 主机名和证书链验证保持开启。loopback 隔离测试核对原生绑定及安全配置，校园网行为在实际环境验证。

等待 HTTP 响应时，取消会唤醒传输循环；快速检测已有结果后，结束另一项等待。物理通道每次检查采集完整双快照，检查前后确认操作有效，同一检查点复用该次核验结果。DNS、连接建立、发送及返回时仍分别核验。

参考接口：[源接口绑定](https://learn.microsoft.com/en-us/windows/win32/winsock/ipproto-ip-socket-options) · [网卡状态](https://learn.microsoft.com/en-us/windows/win32/api/netioapi/ns-netioapi-mib_if_row2) · [套接字回调](https://curl.se/libcurl/c/CURLOPT_OPENSOCKETFUNCTION.html) · [发送前回调](https://curl.se/libcurl/c/CURLOPT_PREREQFUNCTION.html)
