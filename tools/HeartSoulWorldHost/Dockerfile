# Multi-stage publish for Fly.io (linux-x64). Stephen redeploys; agents do not.
FROM mcr.microsoft.com/dotnet/sdk:8.0 AS build
WORKDIR /src
COPY HeartSoulWorldHost.csproj .
RUN dotnet restore -r linux-x64
COPY . .
RUN dotnet publish HeartSoulWorldHost.csproj \
    -c Release \
    -r linux-x64 \
    --self-contained true \
    -p:PublishSingleFile=true \
    -p:IncludeNativeLibrariesForSelfExtract=true \
    -p:DebugType=None \
    -p:DebugSymbols=false \
    -o /app

FROM mcr.microsoft.com/dotnet/runtime-deps:8.0
WORKDIR /app
COPY --from=build /app .
EXPOSE 27770
ENTRYPOINT ["./HeartSoulWorldHost"]
