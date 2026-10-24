FROM certbot/certbot@sha256:0e40d51116b9e1058466aa2fd8f342b1d676dfbc17e47877a9e02c3e2460580a

# Install the disposable HTTP/TLS service before the test joins its internal,
# egress-disabled Docker network. Pin direct APK package versions.
RUN apk add --no-cache \
    apache2=2.4.67-r0 \
    apache2-ssl=2.4.67-r0 \
    curl=8.14.1-r2
