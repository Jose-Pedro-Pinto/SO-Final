////////////////////////////////////////////////////////////////////////
//                                                                    //
//            Trabalho II: Sistema de Gestão de Ficheiros             //
//                                                                    //
// Compilação: gcc vfs.c -Wall -lreadline -o vfs                      //
// Utilização: ./vfs [-b[128|256|512|1024]] [-f[7|8|9|10]] FILESYSTEM //
//                                                                    //
////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <readline/readline.h>
#include <readline/history.h>

#define MAXARGS 100
#define CHECK_NUMBER 9999
#define TYPE_DIR 'D'
#define TYPE_FILE 'F'
#define MAX_NAME_LENGHT 20

#define FAT_ENTRIES(TYPE) ((TYPE) == 7 ? 128 : (TYPE) == 8 ? 256 : (TYPE) == 9 ? 512 : 1024)
#define FAT_SIZE(TYPE) (FAT_ENTRIES(TYPE) * sizeof(int))
#define BLOCK(N) (blocks + (N) * sb->block_size)
#define DIR_ENTRIES_PER_BLOCK (sb->block_size / sizeof(dir_entry))

typedef struct command {
  char *cmd;              // string apenas com o comando
  int argc;               // número de argumentos
  char *argv[MAXARGS+1];  // vector de argumentos do comando
} COMMAND;

typedef struct superblock_entry {
  int check_number;   // número que permite identificar o sistema como válido
  int block_size;     // tamanho de um bloco {128, 256 (default), 512 ou 1024 bytes}
  int fat_type;       // tipo de FAT {7, 8 (default), 9 ou 10}
  int root_block;     // número do 1º bloco a que corresponde o diretório raiz
  int free_block;     // número do 1º bloco da lista de blocos não utilizados
  int n_free_blocks;  // total de blocos não utilizados
} superblock;

typedef struct directory_entry {
  char type;                   // tipo da entrada (TYPE_DIR ou TYPE_FILE)
  char name[MAX_NAME_LENGHT];  // nome da entrada
  unsigned char day;           // dia em que foi criada (entre 1 e 31)
  unsigned char month;         // mes em que foi criada (entre 1 e 12)
  unsigned char year;          // ano em que foi criada (entre 0 e 255 - 0 representa o ano de 1900)
  int size;                    // tamanho em bytes (0 se TYPE_DIR)
  int first_block;             // primeiro bloco de dados
} dir_entry;

// variáveis globais
superblock *sb;   // superblock do sistema de ficheiros
int *fat;         // apontador para a FAT
char *blocks;     // apontador para a região dos dados
int current_dir;  // bloco do diretório corrente

// funções auxiliares
COMMAND parse(char *);
void parse_argv(int, char **);
void show_usage_and_exit(void);
void init_filesystem(int, int, char *);
void init_superblock(int, int);
void init_fat(void);
void init_dir_block(int, int);
void init_dir_entry(dir_entry *, char, char *, int, int);
void exec_com(COMMAND);
int cmp_dir(const void * a, const void * b);

// funções de manipulação de diretórios
void vfs_ls(void);
void vfs_mkdir(char *);
void vfs_cd(char *);
void vfs_pwd(void);
void vfs_pwdcdir(int);
void vfs_rmdir(char *);

// funções de manipulação de ficheiros
void vfs_get(char *, char *);
void vfs_put(char *, char *);
void vfs_cat(char *);
void vfs_cp(char *, char *);
void vfs_mv(char *, char *);
void vfs_rm(char *);


int main(int argc, char *argv[]) {
  char *linha;
  COMMAND com;

  parse_argv(argc, argv);
  while (1) {
    if ((linha = readline("vfs$ ")) == NULL)
      exit(0);
    if (strlen(linha) != 0) {
      add_history(linha);
      com = parse(linha);
      exec_com(com);
    }
    free(linha);
  }
  return 0;
}


COMMAND parse(char *linha) {
  int i = 0;
  COMMAND com;

  com.cmd = strtok(linha, " ");
  com.argv[0] = com.cmd;
  while ((com.argv[++i] = strtok(NULL, " ")) != NULL);
  com.argc = i;
  return com;
}


void parse_argv(int argc, char *argv[]) {
  int i, block_size, fat_type;

  // valores por omissão
  block_size = 256;
  fat_type = 8;
  if (argc < 2 || argc > 4) {
    printf("vfs: invalid number of arguments\n");
    show_usage_and_exit();
  }
  for (i = 1; i < argc - 1; i++) {
    if (argv[i][0] == '-') {
      if (argv[i][1] == 'b') {
	block_size = atoi(&argv[i][2]);
	if (block_size != 128 && block_size != 256 && block_size != 512 && block_size != 1024) {
	  printf("vfs: invalid block size (%d)\n", block_size);
	  show_usage_and_exit();
	}
      } else if (argv[i][1] == 'f') {
	fat_type = atoi(&argv[i][2]);
	if (fat_type != 7 && fat_type != 8 && fat_type != 9 && fat_type != 10) {
	  printf("vfs: invalid fat type (%d)\n", fat_type);
	  show_usage_and_exit();
	}
      } else {
	printf("vfs: invalid argument (%s)\n", argv[i]);
	show_usage_and_exit();
      }
    } else {
      printf("vfs: invalid argument (%s)\n", argv[i]);
      show_usage_and_exit();
    }
  }
  init_filesystem(block_size, fat_type, argv[argc-1]);
  return;
}


void show_usage_and_exit(void) {
  printf("Usage: vfs [-b[128|256|512|1024]] [-f[7|8|9|10]] FILESYSTEM\n");
  exit(1);
}


void init_filesystem(int block_size, int fat_type, char *filesystem_name) {
  int fsd, filesystem_size;

  if ((fsd = open(filesystem_name, O_RDWR)) == -1) {
    // o sistema de ficheiros não existe --> é necessário criá-lo e formatá-lo
    if ((fsd = open(filesystem_name, O_CREAT | O_TRUNC | O_RDWR, S_IRWXU)) == -1) {
      printf("vfs: cannot create filesystem (%s)\n", filesystem_name);
      show_usage_and_exit();
    }

    // calcula o tamanho do sistema de ficheiros
    filesystem_size = block_size + FAT_SIZE(fat_type) + FAT_ENTRIES(fat_type) * block_size;
    printf("vfs: formatting virtual file-system (%d bytes) ... please wait\n", filesystem_size);

    // estende o sistema de ficheiros para o tamanho desejado
    lseek(fsd, filesystem_size - 1, SEEK_SET);
    write(fsd, "", 1);

    // faz o mapeamento do sistema de ficheiros e inicia as variáveis globais
    if ((sb = (superblock *) mmap(NULL, filesystem_size, PROT_READ | PROT_WRITE, MAP_SHARED, fsd, 0)) == MAP_FAILED) {
      close(fsd);
      printf("vfs: cannot map filesystem (mmap error)\n");
      exit(1);
    }
    fat = (int *) ((unsigned long int) sb + block_size);
    blocks = (char *) ((unsigned long int) fat + FAT_SIZE(fat_type));
    
    // inicia o superblock
    init_superblock(block_size, fat_type);
    
    // inicia a FAT
    init_fat();
    
    // inicia o bloco do diretório raiz '/'
    init_dir_block(sb->root_block, sb->root_block);
  } else {
    // calcula o tamanho do sistema de ficheiros
    struct stat buf;
    stat(filesystem_name, &buf);
    filesystem_size = buf.st_size;

    // faz o mapeamento do sistema de ficheiros e inicia as variáveis globais
    if ((sb = (superblock *) mmap(NULL, filesystem_size, PROT_READ | PROT_WRITE, MAP_SHARED, fsd, 0)) == MAP_FAILED) {
      close(fsd);
      printf("vfs: cannot map filesystem (mmap error)\n");
      exit(1);
    }
    fat = (int *) ((unsigned long int) sb + sb->block_size);
    blocks = (char *) ((unsigned long int) fat + FAT_SIZE(sb->fat_type));

    // testa se o sistema de ficheiros é válido 
    if (sb->check_number != CHECK_NUMBER || filesystem_size != sb->block_size + FAT_SIZE(sb->fat_type) + FAT_ENTRIES(sb->fat_type) * sb->block_size) {
      munmap(sb, filesystem_size);
      close(fsd);
      printf("vfs: invalid filesystem (%s)\n", filesystem_name);
      show_usage_and_exit();
    }
  }
  close(fsd);

  // inicia o diretório corrente
  current_dir = sb->root_block;
  return;
}


void init_superblock(int block_size, int fat_type) {
  sb->check_number = CHECK_NUMBER;
  sb->block_size = block_size;
  sb->fat_type = fat_type;
  sb->root_block = 0;
  sb->free_block = 1;
  sb->n_free_blocks = FAT_ENTRIES(fat_type) - 1;
  return;
}


void init_fat(void) {
  int i;

  fat[0] = -1;
  for (i = 1; i < sb->n_free_blocks; i++)
    fat[i] = i + 1;
  fat[sb->n_free_blocks] = -1;
  return;
}


void init_dir_block(int block, int parent_block) {
  dir_entry *dir = (dir_entry *) BLOCK(block);
  // o número de entradas no diretório (inicialmente 2) fica guardado no campo size da entrada "."
  init_dir_entry(&dir[0], TYPE_DIR, ".", 2, block);
  init_dir_entry(&dir[1], TYPE_DIR, "..", 0, parent_block);
  return;
}


void init_dir_entry(dir_entry *dir, char type, char *name, int size, int first_block) {
  time_t cur_time = time(NULL);
  struct tm *cur_tm = localtime(&cur_time);

  dir->type = type;
  strcpy(dir->name, name);
  dir->day = cur_tm->tm_mday;
  dir->month = cur_tm->tm_mon + 1;
  dir->year = cur_tm->tm_year;
  dir->size = size;
  dir->first_block = first_block;
  return;
}


void exec_com(COMMAND com) {
  // para cada comando invocar a função que o implementa
  if (!strcmp(com.cmd, "exit")) {
    exit(0);
  } else if (!strcmp(com.cmd, "ls")) {
    if (com.argc > 1)
      printf("ERROR(input: 'ls' - too many arguments)\n");
    else
      vfs_ls();
  } else if (!strcmp(com.cmd, "mkdir")) {
    if (com.argc < 2)
      printf("ERROR(input: 'mkdir' - too few arguments)\n");
    else if (com.argc > 2)
      printf("ERROR(input: 'mkdir' - too many arguments)\n");
    else
      vfs_mkdir(com.argv[1]);
  } else if (!strcmp(com.cmd, "cd")) {
    if (com.argc < 2)
      printf("ERROR(input: 'cd' - too few arguments)\n");
    else if (com.argc > 2)
      printf("ERROR(input: 'cd' - too many arguments)\n");
    else
      vfs_cd(com.argv[1]);
  } else if (!strcmp(com.cmd, "pwd")) {
    if (com.argc != 1)
      printf("ERROR(input: 'pwd' - too many arguments)\n");
    else
      vfs_pwd();
  } else if (!strcmp(com.cmd, "rmdir")) {
    if (com.argc < 2)
      printf("ERROR(input: 'rmdir' - too few arguments)\n");
    else if (com.argc > 2)
      printf("ERROR(input: 'rmdir' - too many arguments)\n");
    else
      vfs_rmdir(com.argv[1]);
  } else if (!strcmp(com.cmd, "get")) {
    if (com.argc < 3)
      printf("ERROR(input: 'get' - too few arguments)\n");
    else if (com.argc > 3)
      printf("ERROR(input: 'get' - too many arguments)\n");
    else
      vfs_get(com.argv[1], com.argv[2]);
  } else if (!strcmp(com.cmd, "put")) {
    if (com.argc < 3)
      printf("ERROR(input: 'put' - too few arguments)\n");
    else if (com.argc > 3)
      printf("ERROR(input: 'put' - too many arguments)\n");
    else
      vfs_put(com.argv[1], com.argv[2]);
  } else if (!strcmp(com.cmd, "cat")) {
    if (com.argc < 2)
      printf("ERROR(input: 'cat' - too few arguments)\n");
    else if (com.argc > 2)
      printf("ERROR(input: 'cat' - too many arguments)\n");
    else
      vfs_cat(com.argv[1]);
  } else if (!strcmp(com.cmd, "cp")) {
    if (com.argc < 3)
      printf("ERROR(input: 'cp' - too few arguments)\n");
    else if (com.argc > 3)
      printf("ERROR(input: 'cp' - too many arguments)\n");
    else
      vfs_cp(com.argv[1], com.argv[2]);
  } else if (!strcmp(com.cmd, "mv")) {
    if (com.argc < 3)
      printf("ERROR(input: 'mv' - too few arguments)\n");
    else if (com.argc > 3)
      printf("ERROR(input: 'mv' - too many arguments)\n");
    else
      vfs_mv(com.argv[1], com.argv[2]);
  } else if (!strcmp(com.cmd, "rm")) {
    if (com.argc < 2)
      printf("ERROR(input: 'rm' - too few arguments)\n");
    else if (com.argc > 2)
      printf("ERROR(input: 'rm' - too many arguments)\n");
    else
      vfs_rm(com.argv[1]);
  } else
    printf("ERROR(input: command not found)\n");
  return;
}


// ls - lista o conteúdo do diretório actual
void vfs_ls(void) {
  int cblock = current_dir;
  dir_entry *cur_dir = (dir_entry *) BLOCK(cblock);
  int n_entries = cur_dir[0].size;
  while (cblock!=-1){
    int ents = DIR_ENTRIES_PER_BLOCK;
    if(n_entries < DIR_ENTRIES_PER_BLOCK)
      ents = n_entries;
    n_entries -= DIR_ENTRIES_PER_BLOCK;
    qsort(cur_dir,ents,sizeof(dir_entry),cmp_dir);
    for (int i=0;i<ents;i++){
      printf("%s \t%d-%d-%d",cur_dir[i].name,cur_dir[i].day,cur_dir[i].month,cur_dir[i].year+1900);
      if (cur_dir[i].type == TYPE_DIR)
	printf(" [DIR] \n");
      else
	printf(" %d\n",cur_dir[i].size);
    }
    cblock=fat[cblock];
    cur_dir = (dir_entry *) BLOCK(cblock);
  }
  return;
}

//used to compare and sort dir_entries
int cmp_dir(const void * a, const void * b){
  return (strcmp(((dir_entry *) a)->name,((dir_entry *) b)->name));
}


// mkdir dir - cria um subdiretório com nome dir no diretório actual
void vfs_mkdir(char *nome_dir) {
  if (sb->n_free_blocks == 0){
    printf("filesystem full\n");
    return;
  }
  if (strlen(nome_dir)>MAX_NAME_LENGHT){
    printf("name size exceeds limit\n");
    return;	 
  }
  int cblock = current_dir;
  dir_entry *cur_dir = (dir_entry *) BLOCK(cblock);
  dir_entry *cur_dir1 = (dir_entry *) BLOCK(cblock);
  int n_entries = cur_dir[0].size;
  while (cblock !=-1){

    int ents = DIR_ENTRIES_PER_BLOCK;
    if(n_entries < DIR_ENTRIES_PER_BLOCK)
      ents = n_entries;
    n_entries -= DIR_ENTRIES_PER_BLOCK;
    
    for (int i=0;i<ents;i++){
      if (strcmp(nome_dir,cur_dir1[i].name)==0){
	printf("directory with same name exists\n");
	return;
      }
    }
    if(fat[cblock] != -1) {
      cblock=fat[cblock];
      cur_dir1 = (dir_entry *) BLOCK(cblock);
    }
    else
      break;
  }
  if (n_entries == 0){
	int freeblock = sb->free_block;
	sb->n_free_blocks--;
	sb->free_block=fat[freeblock];
	fat[cblock]=freeblock;
	fat[freeblock]=-1;
	
	cur_dir = (dir_entry *) BLOCK(freeblock);
	init_dir_entry(&cur_dir[0],TYPE_DIR, nome_dir, 0, sb->free_block);

	freeblock = sb->free_block;
	sb->free_block=fat[sb->free_block];
	fat[freeblock]=-1;
	sb->n_free_blocks--;
	
	init_dir_block(freeblock, current_dir);

	cur_dir = (dir_entry *) BLOCK(current_dir);
	cur_dir[0].size++;
	return;
    }
  int freeblock = sb->free_block;
  sb->n_free_blocks--;
  sb->free_block=fat[freeblock];
  fat[freeblock]=-1;
  init_dir_entry(&cur_dir1[cur_dir[0].size % DIR_ENTRIES_PER_BLOCK],TYPE_DIR, nome_dir, 0, freeblock);
  init_dir_block(freeblock,current_dir);
  cur_dir[0].size++;
  return;
}


// cd dir - move o diretório actual para dir
void vfs_cd(char *nome_dir) {
  int cblock = current_dir;
  dir_entry *cur_dir = (dir_entry *) BLOCK(cblock);
  int n_entries = cur_dir[0].size;
  while (cblock !=-1){
    int ents = DIR_ENTRIES_PER_BLOCK;
    if(n_entries < DIR_ENTRIES_PER_BLOCK)
      ents = n_entries;
    n_entries -= DIR_ENTRIES_PER_BLOCK;
    
    for (int i=0;i<ents;i++){
      if (strcmp(nome_dir,cur_dir[i].name)==0){
	current_dir = cur_dir[i].first_block;
	return;
      }
    }
    if(fat[cblock] != -1) {
      cblock=fat[cblock];
      cur_dir = (dir_entry *) BLOCK(cblock);
    }
    else
      break;
  }
  printf("no such directory\n");
  return;
}


// pwd - escreve o caminho absoluto do diretório actual
void vfs_pwd(void) {
  vfs_pwdcdir(current_dir);
  printf("\n");
  return;
}
void vfs_pwdcdir(int child) {
  dir_entry *cur_dir = (dir_entry *) BLOCK(current_dir);
  if (cur_dir[0].first_block==0){
    printf("/Root");
    return;
  }
  int hold = current_dir;
  vfs_cd("..");
  vfs_pwdcdir(current_dir);
  int cblock = current_dir;
  cur_dir = (dir_entry *) BLOCK(current_dir);
  int n_entries = cur_dir[0].size;
  while (cblock !=-1){
    int ents = DIR_ENTRIES_PER_BLOCK;
    if(n_entries < DIR_ENTRIES_PER_BLOCK)
      ents = n_entries;
    n_entries -= DIR_ENTRIES_PER_BLOCK;
    
    for (int i=0;i<ents;i++){
      if (cur_dir[i].first_block==hold){
	printf("/%s",cur_dir[i].name);
	vfs_cd(cur_dir[i].name);
	return;
      }
    }
    if(fat[cblock] != -1) {
      cblock=fat[cblock];
      cur_dir = (dir_entry *) BLOCK(cblock);
    }
    else
      break;
  }
  current_dir = hold;
}


// rmdir dir - remove o subdiretório dir (se vazio) do diretório actual
void vfs_rmdir(char *nome_dir) {
  int cblock = current_dir;
  dir_entry *cur_dir1 = (dir_entry *) BLOCK(current_dir);
  dir_entry *cur_dir = cur_dir1;
  int n_entries = cur_dir[0].size;
  int prev = cblock;
  if (strcmp(nome_dir,"..")==0 || strcmp(nome_dir,".")==0){
    printf("directiries '.' and '..' cannot be removed\n");
    return;
  }
  while (cblock !=-1){
    int ents = DIR_ENTRIES_PER_BLOCK;
    if(n_entries < DIR_ENTRIES_PER_BLOCK)
      ents = n_entries;
    n_entries -= DIR_ENTRIES_PER_BLOCK;
    
    for (int i=0;i<ents;i++){
      if (strcmp(cur_dir[i].name,nome_dir)==0){
	//removes
	dir_entry *dir = (dir_entry *) BLOCK(cur_dir[i].first_block);
	if (dir[0].size>2){
	  printf("target directory is not empty, to remove it empty it\n");
	  return;
	}
	printf("%d\n",i);
	int lastblock = cblock;
	for(;fat[lastblock]!=-1;lastblock=fat[lastblock]){
	   prev = lastblock;
	}
	printf("%d\n%d\n",cblock,lastblock);
	dir_entry *lastdir = (dir_entry*) BLOCK(lastblock);
	int j = (cur_dir1[0].size-1) % DIR_ENTRIES_PER_BLOCK;
	if (j==0){
	  fat[prev]=-1;
	  fat[lastblock]=sb->free_block;
	  sb->n_free_blocks++;
	  sb->free_block=lastblock;
	}
	fat[cur_dir[i].first_block]=sb->free_block;
	sb->free_block=cur_dir[i].first_block;
	sb->n_free_blocks++;

	
	printf("%d\n",j);
	init_dir_entry(&cur_dir[i],lastdir[j].type, lastdir[j].name, lastdir[j].size, lastdir[j].first_block);
	cur_dir[i].day = lastdir[j].day;
	cur_dir[i].month = lastdir[j].month;
	cur_dir[i].year = lastdir[j].year;
	cur_dir1[0].size--;
	return;
      }
    }
    if(fat[cblock] != -1) {
      cblock=fat[cblock];
      cur_dir = (dir_entry *) BLOCK(cblock);
    }
    else
      break;
  }
  printf("No such directory\n");
  return;
}


// get fich1 fich2 - copia um ficheiro normal UNIX fich1 para um ficheiro no nosso sistema fich2
void vfs_get(char *nome_orig, char *nome_dest) {
  int fd;
  int cblock = current_dir;
  dir_entry *cur_dir = (dir_entry *) BLOCK(current_dir);
  int n_entries = cur_dir[0].size;
  struct stat buf;
  if ((fd=open(nome_orig,O_RDONLY))==-1){
    printf("NO such File\n");
    return;
  }
  if (lstat(nome_orig,&buf) < 0) {printf("Error");return;}
  if(S_ISDIR(buf.st_mode)){printf("target is a directory,chose a file\n");return;}
  int nblocks = buf.st_size/sb->block_size+1;
  if(nblocks > sb->n_free_blocks){
    printf("Way to big\n");
    return;
  }
  while (cblock !=-1){
    int ents = DIR_ENTRIES_PER_BLOCK;
    if(n_entries < DIR_ENTRIES_PER_BLOCK)
      ents = n_entries;
    n_entries -= DIR_ENTRIES_PER_BLOCK;
    
    for (int i=0;i<ents;i++){
      if (strcmp(cur_dir[i].name,nome_dest)==0){
	printf("file with same name exists\n");
	return;
      }
    }
    if(fat[cblock] != -1) {
      cblock=fat[cblock];
      cur_dir = (dir_entry *) BLOCK(cblock);
    }
    else{
      if (n_entries == 0){
	int freeblock = sb->free_block;
	sb->n_free_blocks--;
	sb->free_block=fat[freeblock];
	fat[cblock]=freeblock;
	fat[freeblock]=-1;
	ents=0;
	cur_dir = (dir_entry *) BLOCK(current_dir);
	cur_dir[0].size++;
	
	cur_dir = (dir_entry *) BLOCK(freeblock);
	
      }
      int  freeblock = sb->free_block;
      sb->free_block=fat[sb->free_block];
      fat[freeblock]=-1;
      sb->n_free_blocks--;
      init_dir_entry(&cur_dir[ents],TYPE_FILE, nome_dest, 0, freeblock);
      cur_dir[ents].size=buf.st_size;
      cur_dir = (dir_entry *) BLOCK(current_dir);
      cur_dir[0].size++;
      cblock = freeblock;
      break;
    }
  }
  read(fd,BLOCK(cblock),sb->block_size);
  
  for (int i=1;i<nblocks;i++){
    int freeblock = sb->free_block;
    sb->n_free_blocks--;
    sb->free_block=fat[freeblock];
    fat[cblock]=freeblock;
    fat[freeblock]=-1;
    cblock = freeblock;
    read(fd,BLOCK(cblock),sb->block_size);
  }
  return;
}


// put fich1 fich2 - copia um ficheiro do nosso sistema fich1 para um ficheiro normal UNIX fich2
void vfs_put(char *nome_orig, char *nome_dest) {
  
  int fd;
  int cblock = current_dir;
  dir_entry *cur_dir = (dir_entry *) BLOCK(current_dir);
  int n_entries = cur_dir[0].size;
  if ((fd=open(nome_orig,O_WRONLY|O_CREAT|O_EXCL|O_TRUNC,S_IRWXU))==-1){
    printf("file with same name exists\n");
    return;
  }
  while (cblock !=-1){
    int ents = DIR_ENTRIES_PER_BLOCK;
    if(n_entries < DIR_ENTRIES_PER_BLOCK)
      ents = n_entries;
    n_entries -= DIR_ENTRIES_PER_BLOCK;
    for (int i=0;i<ents;i++){
      if (strcmp(cur_dir[i].name,nome_dest)==0){
	if(cur_dir[i].type==TYPE_DIR){printf("target is a directory,chose a file\n");return;}
	int cblock = cur_dir[i].first_block;
	while (cblock!=-1){
	  write(fd,BLOCK(cblock),sb->block_size);
	  cblock = fat[cblock];
	}
	return;
      }
    }
    if(fat[cblock] != -1) {
      cblock=fat[cblock];
      cur_dir = (dir_entry *) BLOCK(cblock);
    }
  }
  printf("no such file\n");
  return;

}


// cat fich - escreve para o ecrã o conteúdo do ficheiro fich
void vfs_cat(char *nome_fich) {
  int cblock = current_dir;
  dir_entry *cur_dir = (dir_entry *) BLOCK(current_dir);
  int n_entries = cur_dir[0].size;
  while (cblock !=-1){
    int ents = DIR_ENTRIES_PER_BLOCK;
    if(n_entries < DIR_ENTRIES_PER_BLOCK)
      ents = n_entries;
    n_entries -= DIR_ENTRIES_PER_BLOCK;
    for (int i=0;i<ents;i++){
      if (strcmp(cur_dir[i].name,nome_fich)==0){
	if(cur_dir[i].type==TYPE_DIR){printf("target is a directory,chose a file");return;}
	int cblock = cur_dir[i].first_block;
	while (cblock!=-1){
	  write(STDOUT_FILENO, BLOCK(cblock),sb->block_size);
	  cblock = fat[cblock];
	}
	return;
      }
    }
    if(fat[cblock] != -1) {
      cblock=fat[cblock];
      cur_dir = (dir_entry *) BLOCK(cblock);
    }
  }
  return;
}


// cp fich1 fich2 - copia o ficheiro fich1 para fich2
// cp fich dir - copia o ficheiro fich para o subdiretório dir
void vfs_cp(char *nome_orig, char *nome_dest) {
  return;
}


// mv fich1 fich2 - move o ficheiro fich1 para fich2
// mv fich dir - move o ficheiro fich para o subdiretório dir
void vfs_mv(char *nome_orig, char *nome_dest) {
  return;
}


// rm fich - remove o ficheiro fich
void vfs_rm(char *nome_fich) {
  return;
}
