# Build stage
FROM node:22-alpine AS build
WORKDIR /app
COPY package*.json ./
RUN npm ci
COPY . .
RUN npm run build

# Production stage
FROM nginx:stable-alpine
COPY --from=build /app/dist /usr/share/nginx/html/admin
# The server block is a template: the image entrypoint renders it at
# container start, so a deployment can point the console at another backend
# with FULLA_BACKEND_UPSTREAM instead of rebuilding the image. Only FULLA_*
# variables are substituted; the nginx variables in the file stay as they are.
COPY nginx.conf.template /etc/nginx/templates/default.conf.template
ENV FULLA_BACKEND_UPSTREAM=http://fulla-backend:5555 \
    NGINX_ENVSUBST_FILTER=^FULLA_
EXPOSE 80
CMD ["nginx", "-g", "daemon off;"]
