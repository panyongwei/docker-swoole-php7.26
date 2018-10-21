# Dockerfile 安装swoole和php7.2.6

供新手学习Dockerfile

学习Dockerfile的日常使用指令



分类 | 指令
---|:--|---
基于基础镜像 | FROM
维护者信息 | MAINTAINER
镜像操作指令 | RUN、COPY、ADD、EXPOSE、ENV、WORKDIR
容器启动时执行指令 | CMD、ENTRYPOINT、VOLUME

指令 | 说明
---|:--|---
FROM | 基于哪一个基础镜像来构建，FROM指令必须为第一行
MAINTAINER | 作者信息，Dockerfile维护者信息
RUN | 执行shell命令，当命令过长可以使用 \ 换行，使用方式跟linux的shell一样
COPY | 从宿主机复制文件到容器
ADD | 从宿主机复制文件到容器，和COPY指令差不多，但是会自动处理，例如 tar 包会自动解压，URL会自动下载
EXPOSE | 容器对外开放的端口，启动容器的时候使用 -P 不需要 -p 80:80了
ENV | 配置环境变量，由于容器在结束之后会释放掉因此不能写到/etc/profile或者 ~/.bashrc 中，通过ENV可以解决这类问题
WORKDIR | 设置登录容器的默认目录，一个落脚点
VOLUME | 容器挂载点，用于跟外部宿主机或者其他容器交互数据



以上为常用的Dockerfile分类指令，有一些不常用的就不罗列出来了

### FROM指令

##### 使用方式：
> FROM \<image> 或者 \<image>:\<tag>


```
FROM centos
```
或者

```
FROM centos:latest
```

### MAINTAINER指令
##### 使用方式：
> MAINTAINER 作者信息

```
MAINTAINER sunny <admin@sunnyos.com>
```

### RUN指令
##### 使用方式：
> RUN shell命令

```
RUN yum update -y
```
### COPY指令
##### 使用方式：
> COPY \<src> \<dest>

假如我要把跟 Dockerfile 文件所在目录下的 php-7.2.6.tar.gz 复制到 /www 下面，当容器的/www目录不存在时会自动创建

```
COPY php-7.2.6.tar.gz /www/php-7.2.6.tar.gz
```

### ADD指令
##### 使用方式：
> ADD \<src> \<dest>

ADD 命令等使用和功能都跟COPY一样，只不过这是一个增强版，对于url地址可以自动下载，对于tar压缩包可以自动解压

```
ADD php-7.2.6.tar.gz /tmp 
```
这条命令会把当前目录都php-7.2.6.tar.gz自动解压到容器的 /tmp 目录下

```
ADD http://nginx.org/download/nginx-1.15.5.tar.gz /tmp/nginx-1.15.5.tar.gz

```
这条命令会自动下载 nginx-1.15.5.tar.gz 到 /tmp 下

### EXPOSE指令
##### 使用方式：
> EXPOSE \<port> [\<port>]

对外暴露80和443端口在启动容器的时候直接通过 -P 就会自动生成随机端口指向 EXPOSE 暴露的端口可以通过 docker ps查看

```
EXPOSE 80 443
```

### ENV指令
##### 使用方式：
> ENV <环境变量key> <环境变量值>
```
ENV SUNNY sunny
```

设置一个环境变量为 SUNNY 值为 sunny，然后进入容器执行 echo $SUNNY 查看结果

当容器停止时容器的资源和环境都会被释放掉，我们不能设置 /etc/profile 或者 ~/.bashrc 就可以通过 ENV指令 来设置

### WORKDIR指令
##### 使用方式：
> WORKDIR \<容器目录>

```
WORKDIR /www
```

这条指令会让我们执行命令 docker run -it 进入的时候默认的路径就是在 /www 里面

### VOLUME指令
##### 使用方式：
> VOLUME \<容器目录> 或者 VOLUME [\<容器目录>,\<容器目录>]

使用这个命令在容器外面更改的文件数据在容器里面也会跟着改变

```
VOLUME /www
```
这条命令创建一个挂载点为 /www

```
VOLUME ["/www","/data"]
```

这条命令创建两个挂载点分别为 /www 和 /data 通过 docker inspect 命令可以看到挂载的宿主机所在的路径

我们可以在启动容器的时候使用 -v 参数指定目录， 例如指定我指定我宿主机的 /data路径指向到容器的/www

```
docker run -it -v /data:/www test
```

### CMD指令
##### 使用方式：
> CMD \<shell命令> 或者 CMD [\<shell命令>,\<命令参数>,[命令参数...]]

```
CMD php /www/demo.php

或者

CMD ["php","/www/demo.php"]
```
容器启动的时候执行的命令，当Dockerfile存在多个CMD指令时，只有最后一条会被执行且会被docker run的参数覆盖


### ENTRYPOINT指令
##### 使用方式：
> ENTRYPOINT \<shell命令> 或者 ENTRYPOINT [\<shell命令>,\<命令参数>,[命令参数...]]

```
ENTRYPOINT php /www/demo.php

或者

ENTRYPOINT ["php","/www/demo.php"]
```

ENTRYPOINT 是 CMD 的高级版，不会被docker run的参数覆盖