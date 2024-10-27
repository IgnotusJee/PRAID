# 虚拟磁盘阵列项目

实现一个 raid4 阵列，校验计算设备为模拟的 pcie 设备。

## 组成

* device：pcie 虚拟设备，模拟的 bar 区域的 layout 为偏移0处是`struct pciev_bar`，偏移 1MB 处三个相邻的 stripe (4kb)，分别代表计算奇偶校验的时候对应的旧数据，新数据，原来的校验数据。运行一个线程`pciev_dispatcher`来执行校验的计算。

* block：面向文件系统的块设备，不使用 muti-queue 机制，直接注册`.submit_bio`接口作为`struct bio`的处理函数，上层调用`submit_bio`函数后会直接调用这个接口不会进入队列机制。将`struct bio`按照 stripe 使用`bio_split`拆分为若干面向单个 nvme 设备的小`struct bio`。如果当前操作为‘写’，则提交小的 bio 之前要修改校验盘对应位置上的校验数据。

* pciedrv：pcie 驱动，校验操作的主要执行模块。接受 bio 参数后，将 bio 中每一段的`struct page`的信息拷贝出来作为新的数据，再读出老的数据和老的校验数据之后拷贝到 bar 区域，通知 device 进行校验计算。

## 流程

执行流程中，虚拟 pcie 设备被划分为四个状态，状态储存在`struct pciev_bar`里面。

1. DB_FREE：pcie 设备空闲，准备等待校验任务的写入
2. DB_WRITE：pcie 设备被某个线程抢占，该线程正在执行读取设备数据和将数据写入 bar 空间的任务
3. DB_BUSY：pcie 设备可以进行或者正在进行校验计算任务
4. DB_DONE：pcie 设备任务执行完成，触发中断通知驱动唤醒等待线程，回到 DB_FREE 状态。

校验执行的流程如下：

1. 上级程序提交 bio，被`vpciedisk_submit_bio`函数接收，把大的 bio 分成小的 bio 提交给各个设备。如果为写属性，则在提交给块设备之前提交校验。

2. `pcievdrv_submit_verify`函数遍历 bio 中的每一个段，段中包含了 page, size, offset。对于每个段，如果设备不空闲则加入等待队列开始等待，否则执行该段的校验。

3. `do_verify_task`函数将设备状态置为`DB_WRITE`，并开始往 bar 空间里写入任务信息。首先填充`struct pciev_bar`的参数信息，然后分别提交对应数据盘和校验盘的相应位置 bio，bio 执行完成后将三个`struct page`中的信息拷贝到 bar 空间中的相应区域，将设备状态设置为`DB_BUSY`。

4. 设备循环线程中执行到`pciev_dispatcher_clac_xor_single`函数，检测到设备状态为`DB_BUSY`之后进行校验计算，然后将计算的结果构造 bio 提交到校验盘上，bio 执行完成后将设备状态改为`DB_DONE`，并触发中断。

5. 中断处理函数中将设备状态改为`DB_FREE`并唤醒等待队列中的一个线程。

## 测试

使用`$sudo bash setup.sh`安装模块

采用以下几个指令进行测试

```bash
$ dd if=/dev/zero of=/dev/graiddisk bs=1M count=100000
$ mkfs.ext2 /dev/graiddisk
```

## 问题

上层应用某些 bio 提交之后进入校验环节，其中`do_verify_task`中的`read_stripe_async`（异步）或者`read_stripe_sync`（同步）中，提交构造好的 bio 之后，bio 一直不完成，内核 rcu 发出超时错误。

复现错误的指令为`mkfs.ext2 /dev/graiddisk`或者一个测试的内核模块(biotest/main.c)。

## update 1

上述的问题已经解决，原因是本设备的块设备在处理 bio 的时候，会在提交了下层设备的 bio 之后阻塞在自己的 submit_bio 函数上，此时可能新提交的 bio 和正在处理的 bio 被分配到同一个 kworker pool 里面（也就是同一个 cpu），因为原来的 kworker 阻塞，而这个 pool 里面没有多余的空闲线程资源，所以一直等待他结束获取资源，造成了死锁。

将等待下层 bio 读取该扇区原来数据的阻塞过程通过 kworker 改成了异步的，但是遇到了新的问题，即可能出现该扇区再被读取数据之前先被覆盖的情况出现。如果只是想避免这个 IO 反序的问题的话，只需先提交 read bio 并在它的 endio 处提交相应的 write bio，但是这种方法实现并不优雅（没有利用 bio_split 和 bio_chain 的优点做 bio 的资源管理），而且拓展性差（需要自己写 bio 分离算法，如果 stripe 的大小变化的话算法就要重写）。另一方面，使用驱动一侧读取原数据以及使用额外的 bar 空间作为计算空间并不符合模块的架构（设备侧是虚拟的，应当等同于真实设备看待），所以应该把校验计算的准备过程（数据的准备）放在设备侧进行，这样的话校验的过程就不能和主机写的过程进行同步进行了（这种方式也会降低吞吐量）。

最终的方案是设备侧周期性的处理具有脏、冷数据的 stripe group，这种实现方式和主机侧的 io 是分离的。将在下一 commit 中实现。

目前子目录中的 readtest 目录是直接使用 bio 读取 几个 ssd 设备的头部数据到 dmesg 里面的模块，使用 read.sh 脚本读取和 clearhead.sh 清除头部数据，方便调试。如果使用 dd 命令读取的话，会遇到更新不及时的问题，可能是快设备的缓存导致的。