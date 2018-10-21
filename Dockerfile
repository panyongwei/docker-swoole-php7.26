FROM centos:latest
MAINTAINER sunny<admin@sunnyos.com>

# 设置登录容器的默认目录
WORKDIR /www

# 安装依赖
RUN yum -y install gcc gcc-c++ make cmake automake autoconf kernel-devel ncurses-devel libxml2-devel pcre-devel openssl openssl-devel curl-devel libjpeg-devel libpng-devel  pcre-devel libtool-libs freetype-devel gd zlib-devel file bison bison-devel patch mlocate flex diffutils readline-devel glibc-devel glib2-devel bzip2-devel gettext-devel libcap-devel libmcrypt-devel gmp-devel libxslt-devel git libevent libevent-devel 

# 使用COPY指令需要手动解压
#COPY php-7.2.6.tar.gz /tmp/php-7.2.6.tar.gz
#RUN cd /tmp && tar -zxvf php-7.2.6.tar.gz

# ADD指令会自动处理解压
ADD php-7.2.6.tar.gz /tmp

RUN cd /tmp/php-7.2.6 && \
	./configure --prefix=/usr/local/php \
	--enable-fpm \
	--with-config-file-path=/etc \
	--with-libxml-dir --with-openssl \
	--with-mysqli \
	--with-zlib \
	--enable-bcmath \
	--with-bz2 \
	--enable-calendar \
	--with-curl \
	--enable-exif \
	--with-pcre-dir \
	--enable-ftp \
	--with-openssl-dir \
	--with-gd --with-jpeg-dir \
	--with-png-dir \
	--with-freetype-dir \
	--enable-gd-jis-conv \
	--with-gettext \
	--with-gmp \
	--with-mhash \
	--enable-mbstring \
	--with-libmbfl \
	--with-onig \
	--with-pdo-mysql \
	--with-readline \
	--enable-shmop \
	--enable-soap \
	--enable-sockets \
	--enable-sysvmsg \
	--enable-sysvsem \
	--enable-sysvshm \
	--enable-wddx \
	--with-xmlrpc \
	--with-xsl \
	--enable-zip \
	--with-pear \
	--enable-mysqlnd \
	--enable-shared \
	--enable-inline-optimization \
	--disable-debug \
	--enable-xml \
	--with-sqlite3 \
	--with-iconv \
	--with-cdb \
	--enable-dom \
	--enable-fileinfo \
	--enable-filter \
	--enable-json \
	--enable-mbregex \
	--enable-mbregex-backtrack \
	--enable-pdo \
	--with-pdo-sqlite \
	--enable-session \
	--enable-simplexml \
	--enable-opcache \
	--with-pdo-mysql=mysqlnd && \
	make && make install && \
	cp php.ini-development /etc/php.ini

RUN ln -s /usr/local/php/bin/php /usr/bin/php
RUN ln -s /usr/local/php/bin/phpize /usr/bin/phpize
RUN ln -s /usr/local/php/bin/php-config /usr/bin/php-config
RUN rm -rf /tmp/php*

COPY hiredis /tmp/hiredis

RUN cd /tmp/ && \
	cd hiredis && \
	make clean && make -j && make install

RUN rm -rf hiredis
	

COPY swoole /tmp/swoole

RUN cd /tmp/ && \
	cd swoole && \
	/usr/local/php/bin/phpize && \
	./configure \
	--with-php-config=/usr/local/php/bin/php-config \
	--enable-coroutine \
	--enable-openssl  \
	--enable-async-redis \
	--enable-sockets \
	--enable-mysqlnd && \
	make clean && make && make install

RUN rm -rf /tmp/swoole

# 把swoole扩展写到php.ini里面
RUN echo "extension=swoole.so" >> /etc/php.ini

# 通过RUN指令设置环境变量会失效
#RUN echo "export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib" >> /root/.bashrc && \
#       source /root/.bashrc
# 正确应该通过ENV指令设置环境变量 
ENV LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib

# 在容器里面创建 www 目录
# 启动的时候可以通过 -v 参数指定宿主机映射到容器的目录
VOLUME /www

EXPOSE 9000 9501

# 这个改成适合自己的配置文件
CMD ["php","/www/demo.php"]




