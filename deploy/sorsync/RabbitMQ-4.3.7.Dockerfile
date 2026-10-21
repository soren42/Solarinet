# Test-only intermediate image: official RabbitMQ 4.3.7 generic release over
# the pinned official 4.3.6 image's Erlang 27 runtime. CI verifies the archive
# SHA-256 before extracting it into the Docker build context.
FROM rabbitmq:4.3-management@sha256:54cb013d76cd1fd6c0a0708f34062ad61c452bb6f016b434c6c33f1a765d2b58

USER root
RUN rm -rf /opt/rabbitmq && mkdir -p /opt/rabbitmq
COPY rabbitmq_server-4.3.7/ /opt/rabbitmq/
RUN sed -i 's/^SYS_PREFIX=.*/SYS_PREFIX=/' /opt/rabbitmq/sbin/rabbitmq-defaults \
    && chown -R rabbitmq:rabbitmq /opt/rabbitmq

ENV RABBITMQ_VERSION=4.3.7
