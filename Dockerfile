FROM mcr.microsoft.com/dotnet/sdk:8.0 AS build
WORKDIR /src

COPY DoomLauncher/DoomLauncher.csproj DoomLauncher/
RUN dotnet restore DoomLauncher/DoomLauncher.csproj -r win-x64

COPY DoomLauncher/ DoomLauncher/
RUN dotnet publish DoomLauncher/DoomLauncher.csproj \
    -c Release \
    -r win-x64 \
    -o /app/publish

FROM scratch
COPY --from=build /app/publish /app
